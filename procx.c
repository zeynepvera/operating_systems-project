#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/msg.h>     // System V Message Queue
#include <sys/mman.h>    // POSIX Shared Memory
#include <sys/stat.h>    // POSIX sabitleri
#include <semaphore.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

// ** ENUMERATIONS

typedef enum {
    MODE_ATTACHED = 0,
    MODE_DETACHED = 1
} ProcessMode;

typedef enum {
    STATUS_RUNNING = 0,
    STATUS_TERMINATED = 1
} ProcessStatus;

// ** DATA STRUCTURES

typedef struct {
    pid_t pid;              // Process ID
    pid_t owner_pid;        // Başlatan process'in ID'si
    char command[256];      // Çalıştırılan komut
    ProcessMode mode;       // 0: Attached, 1: Detached
    ProcessStatus status;   // 0: Running, 1: Terminated
    time_t start_time;      // Başlangıç zamanı
    int is_active;          // 1: Aktif, 0: Pasif
} ProcessInfo;

typedef struct {
    ProcessInfo processes[50];  // Maksimum 50 süreç
    int process_count;          // Aktif süreç sayısı

    // Broadcast işlemleri için çalışan ProcX örneklerinin listesi
    pid_t instance_pids[16];    // Maksimum 16 instance
    int instance_count;
} SharedData;

typedef struct {
    long msg_type;        // Mesaj tipi (Hedef Instance ID)
    int command;          // Komut (START/TERMINATE/FINISHED/SHUTDOWN)
    pid_t sender_pid;     // Gönderen PID
    pid_t target_pid;     // Hedef Process PID
} Message;

// ** IPC CONSTANTS

// POSIX Shared Memory ismi
#define SHM_NAME "/procx_shm"

// POSIX Semaphore ismi
#define SEM_NAME "/procx_sem"

// System V Message Queue anahtarı
#define MSG_KEY  0x23456

// Mesaj komut kodları
#define CMD_SHUTDOWN  0
#define CMD_START     1
#define CMD_TERMINATE 2
#define CMD_FINISHED  3

// ** GLOBALS

static int shm_fd = -1;
static SharedData *shared_data = NULL;
static sem_t *shared_sem = NULL;
static int msg_id = -1;

static pthread_t monitor_thread;
static pthread_t ipc_thread;
static volatile int running = 1;

// ** HELPER FUNCTIONS

const char *mode_to_str(ProcessMode m) {
    return (m == MODE_DETACHED) ? "Detached" : "Attached";
}

void lock_shared(void) {
    if (sem_wait(shared_sem) == -1) {
        perror("sem_wait");
    }
}

void unlock_shared(void) {
    if (sem_post(shared_sem) == -1) {
        perror("sem_post");
    }
}

// ** INSTANCE REGISTER / UNREGISTER

// Bu process'i shared memory üzerindeki instance listesine kaydeder
void register_instance(void) {
    pid_t me = getpid();
    int found = 0;

    for (int i = 0; i < 16; i++) {
        if (shared_data->instance_pids[i] == me) {
            found = 1;
            break;
        }
    }

    if (!found) {
        for (int i = 0; i < 16; i++) {
            if (shared_data->instance_pids[i] == 0) {
                shared_data->instance_pids[i] = me;
                shared_data->instance_count++;
                break;
            }
        }
    }
}

// Çıkış yaparken instance kaydını siler
void unregister_instance(void) {
    pid_t me = getpid();

    for (int i = 0; i < 16; i++) {
        if (shared_data->instance_pids[i] == me) {
            shared_data->instance_pids[i] = 0;
            if (shared_data->instance_count > 0) {
                shared_data->instance_count--;
            }
            break;
        }
    }
}

// ** IPC INIT & CLEANUP

void init_ipc(void) {
    int is_creator = 0;

    // 1. SEMAPHORE (POSIX)
    // Semaforu oluştur veya aç (Initial value: 1)
    shared_sem = sem_open(SEM_NAME, O_CREAT, 0666, 1);
    if (shared_sem == SEM_FAILED) {
        perror("sem_open");
        exit(EXIT_FAILURE);
    }

    // 2. SHARED MEMORY (POSIX)
    // O_EXCL ile ilk oluşturanın kim olduğunu kontrol et
    shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR | O_EXCL, 0666);
    
    if (shm_fd != -1) {
        // Dosyayı ilk biz oluşturduk
        is_creator = 1;
        // Boyutu ayarla
        if (ftruncate(shm_fd, sizeof(SharedData)) == -1) {
            perror("ftruncate");
            exit(EXIT_FAILURE);
        }
    } else {
        if (errno == EEXIST) {
            // Zaten varsa sadece aç
            shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
            if (shm_fd == -1) {
                perror("shm_open (exist)");
                exit(EXIT_FAILURE);
            }
        } else {
            perror("shm_open");
            exit(EXIT_FAILURE);
        }
    }

    // Bellek haritalama (Mapping)
    shared_data = (SharedData *)mmap(NULL, sizeof(SharedData), 
                                     PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shared_data == MAP_FAILED) {
        perror("mmap");
        exit(EXIT_FAILURE);
    }

    // 3. MESSAGE QUEUE (System V)
    msg_id = msgget(MSG_KEY, IPC_CREAT | 0666);
    if (msg_id == -1) {
        perror("msgget");
        exit(EXIT_FAILURE);
    }

    // --- BAŞLANGIÇ AYARLARI ---
    lock_shared();
    
    if (is_creator) {
        // İlk başlatan bizsek belleği temizle
        memset(shared_data, 0, sizeof(SharedData));
    }

    register_instance();

    // Hatalı kapanmış (stale) process kayıtlarını temizle
    for (int i = 0; i < 50; i++) {
        ProcessInfo *p = &shared_data->processes[i];
        if (!p->is_active) continue;

        // Process gerçekten hayatta mı kontrol et
        if (kill(p->pid, 0) == -1 && errno == ESRCH) {
            p->is_active = 0;
            p->status = STATUS_TERMINATED;
            if (shared_data->process_count > 0) {
                shared_data->process_count--;
            }
        }
    }

    unlock_shared();
}

void cleanup_ipc(void) {
    if (shared_data != MAP_FAILED && shared_sem != SEM_FAILED) {
        lock_shared();
        unregister_instance();
        int remaining_instances = shared_data->instance_count;
        unlock_shared();

        // Mapping'i kaldır ve descriptorları kapat
        munmap(shared_data, sizeof(SharedData));
        close(shm_fd);
        sem_close(shared_sem);

        // Eğer sistemdeki son instance bizsek kaynakları tamamen sil
        if (remaining_instances == 0) {
            shm_unlink(SHM_NAME); // SHM dosyasını sil
            sem_unlink(SEM_NAME); // Semaforu sil
            msgctl(msg_id, IPC_RMID, NULL); // Mesaj kuyruğunu sil
            printf("[INFO] System resources cleaned up.\n");
        }
    }
}

// ** IPC MESSAGE SENDER (BROADCAST)

void send_message(int cmd, pid_t target_pid) {
    if (msg_id == -1) return;

    pid_t me = getpid();

    lock_shared();
    // Tüm aktif instance'lara mesaj gönder
    for (int i = 0; i < 16; i++) {
        pid_t inst = shared_data->instance_pids[i];
        if (inst == 0) continue;

        Message msg;
        msg.msg_type   = inst;  // Hedef instance ID
        msg.command    = cmd;
        msg.sender_pid = me;
        msg.target_pid = target_pid;

        // Mesajı kuyruğa ekle
        if (msgsnd(msg_id, &msg, sizeof(Message) - sizeof(long), 0) == -1) {
             // Kuyruk doluysa veya hata varsa görmezden gel
        }
    }
    unlock_shared();
}

// ** MONITOR THREAD

// Arka planda biten süreçleri (zombies) tespit edip temizler
void *monitor_thread_func(void *arg) {
    (void)arg;

    while (running) {
        pid_t finished_pids[50];
        int finished_count = 0;

        lock_shared();

        for (int i = 0; i < 50; i++) {
            ProcessInfo *p = &shared_data->processes[i];

            if (!p->is_active) continue;
            if (p->owner_pid != getpid()) continue; // Sadece kendi başlattıklarımızı izle
            if (p->status != STATUS_RUNNING) continue;

            int status;
            pid_t res = waitpid(p->pid, &status, WNOHANG);
            if (res > 0) {
                // Process sonlanmış
                p->status = STATUS_TERMINATED;
                p->is_active = 0;
                if (shared_data->process_count > 0) {
                    shared_data->process_count--;
                }

                printf("\n[MONITOR] Process %d was terminated\n", p->pid);
                if (finished_count < 50) {
                    finished_pids[finished_count++] = p->pid;
                }
            }
        }

        unlock_shared();

        // Biten process bilgisini diğer instance'lara duyur
        for (int i = 0; i < finished_count; i++) {
            send_message(CMD_FINISHED, finished_pids[i]);
        }

        sleep(2); 
    }
    return NULL;
}

// ** IPC LISTENER THREAD

// Gelen mesajları dinler ve ekrana basar
void *ipc_listener_thread_func(void *arg) {
    (void)arg;
    pid_t me = getpid();

    while (running) {
        Message msg;
        // Sadece bana (PID) gelen mesajları al
        ssize_t n = msgrcv(msg_id, &msg, sizeof(Message) - sizeof(long), me, 0);

        if (n == -1) {
            if (errno == EINTR) continue;
            if (errno == EIDRM) break; // Kuyruk silindiyse döngüden çık
            continue;
        }

        // Kapatma sinyali
        if (!running && msg.command == CMD_SHUTDOWN && msg.sender_pid == me) {
            break;
        }

        if (msg.sender_pid == me) continue;

        switch (msg.command) {
            case CMD_START:
                printf("\n[IPC] New process started: PID %d\n", msg.target_pid);
                break;
            case CMD_TERMINATE:
                printf("\n[IPC] Process terminated: PID %d\n", msg.target_pid);
                break;
            case CMD_FINISHED:
                printf("\n[IPC] Process finished: PID %d\n", msg.target_pid);
                break;
            default:
                break;
        }
    }
    return NULL;
}

// ** MENU & MAIN LOGIC

void print_menu(void) {
    printf("╔════════════════════════════════════╗\n");
    printf("║             ProcX v1.0             ║\n");
    printf("╠════════════════════════════════════╣\n");
    printf("║ 1. Run a new program               ║\n");
    printf("║ 2. List running programs           ║\n");
    printf("║ 3. Terminate a program             ║\n");
    printf("║ 0. Exit                            ║\n");
    printf("╚════════════════════════════════════╝\n");
    printf("Your choice: ");
    fflush(stdout);
}

void run_new_program(void) {
    char cmdline[256];
    printf("Enter the command to run: ");
    fflush(stdout);
    
    if (!fgets(cmdline, sizeof(cmdline), stdin)) return;
    cmdline[strcspn(cmdline, "\n")] = '\0';

    if (strlen(cmdline) == 0) return;

    int mode;
    printf("Choose running mode (0: Attached, 1: Detached): ");
    fflush(stdout);
    if (scanf("%d", &mode) != 1) {
        printf("Invalid input.\n");
        while(getchar() != '\n'); 
        return;
    }
    while(getchar() != '\n'); // Buffer temizle

    if (mode != 0 && mode != 1) { printf("Invalid mode.\n"); return; }

    // Argümanları parçala
    char *args[32];
    int i = 0;
    char tmp[256];
    strncpy(tmp, cmdline, sizeof(tmp)-1);
    tmp[sizeof(tmp)-1] = '\0';
    
    char *token = strtok(tmp, " ");
    while (token != NULL && i < 31) {
        args[i++] = token;
        token = strtok(NULL, " ");
    }
    args[i] = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return;
    }

    if (pid == 0) {
        // --- CHILD PROCESS ---
        if (mode == MODE_DETACHED) {
            setsid(); // Detached mod için yeni session
        }
        execvp(args[0], args);
        perror("execvp");
        exit(EXIT_FAILURE);
    } else {
        // --- PARENT PROCESS ---
        // Programın donmaması için waitpid kullanılmıyor, kayıt shared memory'e atılıyor
        lock_shared();
        int slot = -1;
        for (int k = 0; k < 50; k++) {
            if (!shared_data->processes[k].is_active) {
                slot = k;
                break;
            }
        }

        if (slot != -1) {
            ProcessInfo *p = &shared_data->processes[slot];
            p->pid = pid;
            p->owner_pid = getpid();
            strncpy(p->command, cmdline, sizeof(p->command)-1);
            p->mode = (ProcessMode)mode;
            p->status = STATUS_RUNNING;
            p->start_time = time(NULL);
            p->is_active = 1;
            shared_data->process_count++;
        }
        unlock_shared();

        send_message(CMD_START, pid);
        printf("[SUCCESS] Process started: PID %d\n", pid);

        if (mode == MODE_ATTACHED) {
            printf("[INFO] (ATTACHED) Process running linked to this terminal.\n");
        } else {
            printf("[INFO] (DETACHED) Process running in background.\n");
        }
    }
}

void list_processes(void) {
    lock_shared();
    time_t now = time(NULL);
    int active_count = 0;

    printf("\n╔══════════════════════════════════════════════════════════════════════╗\n");
    printf("║                        RUNNING PROGRAMS                              ║\n");
    printf("╠══════════════════════════════════════════════════════════════════════╣\n");
    printf("║ %-6s │ %-20s │ %-9s │ %-7s │ %-6s ║\n", "PID", "Command", "Mode", "Owner", "Time");
    printf("╠══════════════════════════════════════════════════════════════════════╣\n");

    for (int i = 0; i < 50; i++) {
        ProcessInfo *p = &shared_data->processes[i];
        if (p->is_active && p->status == STATUS_RUNNING) {
            active_count++;
            double secs = difftime(now, p->start_time);
            printf("║ %-6d │ %-20s │ %-9s │ %-7d │ %-4lds ║\n",
                   p->pid, p->command, mode_to_str(p->mode), p->owner_pid, (long)secs);
        }
    }
    if (active_count == 0) printf("║ %-68s ║\n", "No running processes.");
    printf("╚══════════════════════════════════════════════════════════════════════╝\n");
    unlock_shared();
}

void terminate_process(void) {
    pid_t pid;
    printf("Process PID to terminate: ");
    fflush(stdout); 
    if (scanf("%d", &pid) != 1) {
        printf("Invalid PID.\n");
        while(getchar() != '\n');
        return;
    }
    while(getchar() != '\n');

    // Kill sinyali gönder
    if (kill(pid, SIGTERM) == 0) {
        printf("[INFO] SIGTERM sent to %d. Removing from list.\n", pid);
        send_message(CMD_TERMINATE, pid);

    } else {
        if (errno == ESRCH) {
             printf("[WARN] Process not found (already dead?), cleaning record.\n");
             lock_shared();
             for (int i=0; i<50; i++) {
                 if(shared_data->processes[i].pid == pid) {
                     shared_data->processes[i].is_active = 0;
                     if(shared_data->process_count > 0) shared_data->process_count--;
                     break;
                 }
             }
             unlock_shared();
             send_message(CMD_TERMINATE, pid);
        } else {
            perror("kill");
        }
    }
}

int main(void) {
    // stdout buffering kapat (anlık çıktı için)
    setvbuf(stdout, NULL, _IONBF, 0);

    init_ipc();

    if (pthread_create(&monitor_thread, NULL, monitor_thread_func, NULL) != 0) {
        perror("pthread_create monitor");
        cleanup_ipc();
        return 1;
    }
    if (pthread_create(&ipc_thread, NULL, ipc_listener_thread_func, NULL) != 0) {
        perror("pthread_create ipc");
        running = 0;
        pthread_join(monitor_thread, NULL);
        cleanup_ipc();
        return 1;
    }

    int menu_running = 1;
    while (menu_running) {
        print_menu();
        int choice;
        if (scanf("%d", &choice) != 1) {
            while(getchar() != '\n'); 
            continue;
        }
        while(getchar() != '\n'); // temizle

        switch (choice) {
            case 1: run_new_program(); break;
            case 2: list_processes(); break;
            case 3: terminate_process(); break;
            case 0: menu_running = 0; break;
            default: printf("Unknown option.\n"); break;
        }
    }

    // ** ÇIKIŞ VE TEMİZLİK
    lock_shared();
    for (int i = 0; i < 50; i++) {
        ProcessInfo *p = &shared_data->processes[i];
        // Attached modda çalışan ve bizim başlattığımız processleri kapat
        if (p->is_active && p->owner_pid == getpid() && p->mode == MODE_ATTACHED) {
            kill(p->pid, SIGTERM);
        }
    }
    unlock_shared();

    running = 0;
    send_message(CMD_SHUTDOWN, 0); // IPC thread'ini uyandır
    
    pthread_join(monitor_thread, NULL);
    pthread_join(ipc_thread, NULL);
    
    cleanup_ipc();
    printf("Exiting ProcX.\n");
    return 0;
}
