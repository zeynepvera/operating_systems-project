# operating_systems-project

# ProcX v1.0

ProcX is a simple process management program written in **C** that allows users to run, list, and terminate processes through a text-based menu interface.

## 🚀 Features

ProcX provides the following functionality:

1. **Run a new program**

   * You can enter any Linux command (e.g., `sleep 100`).
   * You can choose between:

     * **Attached mode (0)** → Process is linked to the terminal.
     * **Detached mode (1)** → Process runs in the background.

2. **List running programs**

   * Displays currently managed processes in a table including:

     * PID
     * Command
     * Running mode (Attached/Detached)
     * Owner
     * Elapsed time

3. **Terminate a program**

   * You can terminate a running process by selecting its PID.

4. **Exit**

   * Closes the ProcX program.

## 🛠️ Building the Project

Make sure you have `gcc` installed, then run:

```bash
make clean
make
```

This will compile the project and generate the executable `procx`.

## ▶️ Running the Program

After compilation, start the program with:

```bash
./procx
```

You will see a menu like this:

```
ProcX v1.0

1. Run a new program
2. List running programs
3. Terminate a program
0. Exit
```

## 📌 Example Usage

### Running a detached process

```
Your choice: 1
Enter the command to run: sleep 100
Choose running mode (0: Attached, 1: Detached): 1
[SUCCESS] Process started: PID 10216
[INFO] (DETACHED) Process running in background.
```

### Listing processes

```
Your choice: 2

PID   | Command   | Mode       | Owner | Time
10216 | sleep 100 | Detached   | 10213 | 9 s
```

### Terminating a process

```
Your choice: 3
Enter PID to terminate: 10216
[MONITOR] Process 10216 was terminated
```

## 🧠 Notes

* This project demonstrates basic concepts such as:

  * Process creation (`fork`, `exec`)
  * Background processes
  * Inter-process communication (IPC)
  * Process monitoring


