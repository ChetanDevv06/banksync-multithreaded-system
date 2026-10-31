# BankSync — Multithreaded Banking & ATM Simulation

<p align="center">
  <strong>A C-based banking simulation demonstrating Operating Systems concepts through concurrent banking operations, thread synchronization, and resource management.</strong>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Language-C11-blue?style=for-the-badge&logo=c" alt="C11">
  <img src="https://img.shields.io/badge/Threads-POSIX%20Pthreads-green?style=for-the-badge" alt="POSIX Threads">
  <img src="https://img.shields.io/badge/Build-CMake-red?style=for-the-badge&logo=cmake" alt="CMake">
  <img src="https://img.shields.io/badge/Status-In%20Development-orange?style=for-the-badge" alt="In Development">
</p>

---

## Overview

**BankSync** is a multithreaded banking and ATM simulation developed in C as part of an Operating Systems academic project.

The project demonstrates how operating system mechanisms manage concurrent execution and shared resources in a banking environment where multiple customers can perform financial operations simultaneously.

It explores practical implementations of POSIX threads, mutex synchronization, race-condition prevention, deadlock avoidance, producer-consumer coordination, and thread management.

Rather than treating Operating Systems concepts as isolated theoretical topics, BankSync applies them to a realistic application scenario involving shared bank accounts, concurrent financial transactions, and transaction history management.

## Objectives

- Demonstrate practical applications of Operating Systems concepts.
- Implement concurrent banking operations using POSIX threads.
- Prevent race conditions and inconsistent account balances.
- Ensure transaction correctness through synchronization mechanisms.
- Explore deadlock conditions and prevention strategies.
- Develop a modular and extensible banking application architecture.
- Validate concurrency and synchronization through systematic testing.

---

## Features

### Implemented

| Feature | Description | Status |
|---|---|---|
| Account Management | Create, initialize, and manage bank accounts | Complete |
| Deposit | Add funds to customer accounts | Complete |
| Withdrawal | Withdraw funds with balance validation | Complete |
| Balance Enquiry | Retrieve account balances safely | Complete |
| Fund Transfer | Transfer funds atomically between accounts | Complete |
| Account Synchronization | Per-account mutex protection | Complete |
| Deadlock Prevention | Consistent account mutex acquisition ordering | Complete |
| Transaction History | Thread-safe transaction recording and retrieval | Complete |
| Reservation Management | Reserve, commit, and cancel transaction-history slots | Complete |
| Concurrent Validation | Multithreaded banking and history stress tests | Complete |

### Planned

- Reusable POSIX worker thread pool.
- Thread-safe transaction queue.
- Concurrent ATM sessions.
- Concurrent online banking sessions.
- Worker lifecycle monitoring.
- Race-condition demonstration environment.
- Deadlock simulation and visualization.
- Graphical user interface using GTK 4.

---

## Technology Stack

| Technology | Purpose |
|---|---|
| C11 | Core application development |
| POSIX Pthreads | Multithreading and thread management |
| Pthread Mutexes | Mutual exclusion and shared-resource protection |
| Pthread Condition Variables | Thread coordination (planned thread-pool implementation) |
| CMake | Build configuration and project management |
| GCC / MinGW GCC | Compilation |
| Windows / Linux | Development and target environments |

---

## System Architecture

BankSync follows a modular architecture that separates the banking engine, synchronization mechanisms, execution infrastructure, and presentation layer.

```text
                 BANKSYNC APPLICATION
                          |
          +---------------+---------------+
          |               |               |
      Presentation   Application     Monitoring
        Layer         Controller        Layer
      (Planned)        (Planned)       (Planned)
          |               |
          +-------+-------+
                  |
          EXECUTION LAYER
             (Planned)
                  |
          +-------+-------+
          |               |
      Thread Pool    Transaction Queue
          |               |
          +-------+-------+
                  |
          BANKING ENGINE
                  |
       +----------+----------+
       |          |          |
    Accounts  Transactions  History
       |          |          |
       +----------+----------+
                  |
        SYNCHRONIZATION LAYER
                  |
       +----------+----------+
       |                     |
  Account Mutexes       History Mutex
       |                     |
       +----------+----------+
                  |
           DATA STORAGE
           (In-Memory)
```

*Note: The banking engine and synchronization components are implemented. The execution, presentation, and monitoring components are part of the development roadmap.*

### Core Banking Operations

The banking engine currently supports:

- Account creation and lookup.
- Deposit and withdrawal.
- Balance enquiry.
- Inter-account fund transfers.
- Transaction recording and retrieval.

Financial amounts are represented using integer paise (`int64_t`) to avoid floating-point precision issues in monetary calculations.

---

## Operating Systems Concepts

BankSync is designed to demonstrate the following Operating Systems concepts through practical implementation.

### 1. Multithreading

Multiple threads can execute banking operations concurrently using POSIX threads.

The concurrency tests create multiple worker threads that perform simultaneous deposits, withdrawals, and fund transfers.

### 2. Mutual Exclusion

Each bank account has an associated `pthread_mutex_t`.

The mutex protects the account balance during critical operations, ensuring that only one thread at a time can modify or read the protected balance.

### 3. Race Condition Prevention

Without synchronization, simultaneous transactions could produce lost updates or inconsistent account balances.

BankSync uses account-level mutexes to protect critical sections and maintain transaction correctness.

### 4. Deadlock Prevention

Fund transfers require access to two account mutexes.

To prevent circular wait, BankSync follows a consistent global lock-ordering strategy:

- Identify the account with the lower account ID.
- Acquire its mutex first.
- Acquire the higher-ID account mutex second.
- Release both locks after completing the transfer.

This prevents deadlocks caused by inconsistent mutex acquisition ordering.

### 5. Producer-Consumer Problem

A bounded transaction queue and worker pool are planned for the execution infrastructure.

The application will act as a producer of banking requests, while worker threads will consume and execute queued transactions.

### 6. Thread Synchronization

The banking engine uses separate synchronization mechanisms for account balances and transaction history.

A dedicated history mutex protects transaction records, transaction identifiers, and reservation state.

---

## Transaction Management

BankSync implements synchronized transaction recording for successful financial operations.

### Supported Transaction Types

- Deposit
- Withdrawal
- Fund Transfer

Balance enquiries do not create financial transaction records.

### Transaction Lifecycle

Transaction history uses a three-state reservation model:

| State | Description |
|---|---|
| FREE | Available for a new transaction |
| RESERVED | Temporarily allocated to an operation |
| COMMITTED | Successfully recorded and visible to readers |

The reservation lifecycle ensures that failed transactions do not permanently consume history capacity.

### Transaction Integrity

- Each successful financial operation produces one transaction record.
- Transfers generate one logical transaction record.
- Failed operations do not produce committed records.
- Transaction IDs are assigned under history synchronization.
- Canceled reservations can be reused.
- History retrieval exposes committed records only.

The current implementation uses a fixed-capacity history of 1,000 records.

---

## Testing and Validation

Testing is an important part of BankSync's development process.

The core banking engine has undergone functional, boundary, financial-invariant, and concurrent testing.

### Test Coverage

| Category | Validation |
|---|---|
| Functional Testing | Account and banking operations |
| Boundary Testing | Zero balances, maximum values, and overflow |
| Financial Invariants | Balance correctness and transfer conservation |
| Concurrent Deposits | Multiple threads depositing into shared accounts |
| Concurrent Withdrawals | Simultaneous withdrawals |
| Concurrent Transfers | Opposite-direction account transfers |
| Mixed Operations | Concurrent deposits and withdrawals |
| History Integrity | Concurrent transaction recording and retrieval |
| Reservation Lifecycle | Reservation cancellation and slot reuse |
| Resource Lifecycle | Initialization, cleanup, and thread joining |

### Latest Verification Results

<details>
<summary><strong>Phase 2 — Core Banking Engine validation</strong></summary>

- 6,373 assertions passed per reported test-suite execution.
- 0 reported failures.
- 10 consecutive successful test-suite executions.
- 10 threads performing concurrent deposit and withdrawal workloads.
- 10 threads performing opposite-direction transfers.
- Concurrent transaction-history readers and writers.
- No arbitrary sleeps used for coordinating worker starts.
- Clean build with strict compiler warning flags.

Compiler flags:

```text
-Wall -Wextra -Wpedantic -Werror
```

**Platform verified:** Windows with MinGW GCC 13.2.0.

Linux/WSL2 sanitizer validation remains pending.

</details>

---

## Project Structure

The project is being developed using a modular C architecture.

```text
BankSync/
│
├── include/
│   └── bank.h
│
├── src/
│   ├── bank.c
│   └── main.c
│
├── tests/
│   └── test_bank.c
│
├── CMakeLists.txt
├── .gitignore
└── README.md
```

Additional modules and test executables will be introduced as development progresses.

---

## Getting Started

### Prerequisites

Install the following tools:

- GCC compiler with POSIX thread support.
- CMake.
- Git.
- Windows MinGW environment or a compatible Linux environment.

### 1. Clone the repository

```bash
git clone https://github.com/ChetanDevv06/banksync-multithreaded-system.git
```

### 2. Navigate to the project directory

```bash
cd banksync-multithreaded-system
```

### 3. Configure the project

For Windows with MinGW Makefiles:

```powershell
cmake -S . -B build -G "MinGW Makefiles"
```

For Linux:

```bash
cmake -S . -B build
```

### 4. Build the application

```bash
cmake --build build
```

### 5. Run the application

Windows:

```powershell
build\banksync.exe
```

Linux:

```bash
./build/banksync
```

### 6. Run the banking engine tests

Windows:

```powershell
build\test_bank.exe
```

Linux:

```bash
./build/test_bank
```

*Build and execution commands correspond to the currently implemented project targets.*

---

## Development Roadmap

BankSync is being developed incrementally through seven major phases.

| Phase | Description | Status |
|---|---|---|
| Phase 1 | Requirements and Architecture Design | Complete |
| Phase 2 | Core Banking Engine | Complete |
| Phase 3 | Multithreading Infrastructure | In Progress |
| Phase 4 | OS Synchronization Experiments | Planned |
| Phase 5 | GTK Graphical Interface | Planned |
| Phase 6 | Integration and Testing | Planned |
| Phase 7 | Documentation and Submission | Planned |

### Phase 2 — Core Banking Engine

- [x] M2.1 — Account Management
- [x] M2.2 — Deposit and Withdrawal
- [x] M2.3 — Balance Enquiry
- [x] M2.4 — Fund Transfer
- [x] M2.5 — Thread-Safe Transaction History
- [x] M2.6 — Banking Engine Testing and Validation

### Phase 3 — Multithreading Infrastructure

- [x] M3.1 — POSIX Thread Pool
- [x] M3.2 — Transaction Queue
- [x] M3.3 — Concurrent ATM Sessions
- [x] M3.4 — Concurrent Online Banking
- [x] M3.5 — Worker Lifecycle Monitoring

### Phase 4 — OS Synchronization Experiments

- [x] M4.1 — Unsafe Execution Demonstration
- [ ] M4.2 — Race Condition Experiment
- [ ] M4.3 — Mutex Synchronization Demonstration
- [ ] M4.4 — Deadlock Simulation
- [ ] M4.5 — Deadlock Prevention
---

## Design Principles

BankSync follows several engineering principles:

- **Modularity:** Keep banking logic independent of presentation and execution infrastructure.
- **Correctness:** Protect shared financial data through appropriate synchronization.
- **Consistency:** Maintain financial invariants across successful transactions.
- **Resource Safety:** Handle mutex initialization, locking, unlocking, and cleanup carefully.
- **Testability:** Validate modules independently before integrating them.
- **Incremental Development:** Implement and verify individual milestones before proceeding.
- **Maintainability:** Prefer clear interfaces, documented assumptions, and minimal unnecessary dependencies.

---

## Current Limitations

The current implementation has the following limitations:

- Banking data is stored in memory and is not persistent.
- Transaction history has a fixed capacity of 1,000 records.
- The GTK interface is not yet implemented.
- The reusable worker pool and transaction queue are under development.
- Linux/WSL2 sanitizer verification is pending.
- Concurrent account creation is not supported by the current account-registry design.

These limitations will be addressed selectively as the project progresses.

---

## Contributing

Contributions, suggestions, and technical feedback are welcome.

If you would like to contribute:

1. Fork the repository.
2. Create a feature branch.
3. Make focused changes.
4. Add or update relevant tests.
5. Verify that the existing test suite passes.
6. Submit a pull request describing your changes.

Please avoid introducing unrelated changes or modifying established synchronization behavior without appropriate testing.

---

## Author

**Chetan**

Computer Science and Engineering Student

Interested in systems programming, software engineering, concurrency, and practical applications of Operating Systems concepts.

- GitHub: [@ChetanDevv06](https://github.com/ChetanDevv06)
- Repository: [BankSync — Multithreaded Banking System](https://github.com/ChetanDevv06/banksync-multithreaded-system)

---

## License

No license has been specified for this repository yet.

---

<p align="center">
  <strong>BankSync</strong>
  <br>
  <em>Understanding Operating Systems through practical engineering.</em>
</p>