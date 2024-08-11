#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

void ecall(uint64_t registers[32]);
void print_instruction(uint32_t);
void prompt();

uint16_t RISCV = 0xf3;

struct ehdr {
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

struct perf {
    uint64_t instruction_count;
    uint64_t loads;
    uint64_t stores;
    uint64_t syscalls;
};

void print_usage(char* prog) {
    fprintf(stderr, "USAGE: %s <EXE>\n\t-d: Debugger mode.\n", prog);
}

uint32_t* pc = 0;
uint32_t* instructions = 0;
struct perf perf = {};
uint64_t registers[32] = {};
int debug = 0;
uint64_t heap_start = 0;
uint64_t heap_end = 0;

void segfault(int sig_num) {
    fprintf(stderr, "SEGFAULT\n");
    while (1) {
        prompt();
    }
}

int main(int argc, char* argv[]) {
    char* prog = argv[0];
    if (argc < 2) {
        print_usage(prog);
        return 1;
    }

    argc--;
    argv++;
    if (strcmp(argv[0], "-d") == 0) {
        debug = 1;
        if (argc == 1) {
            print_usage(prog);
            return 1;
        }
        argc--;
        argv++;
    }

    int exe = openat(AT_FDCWD, argv[0], O_RDONLY);
    struct ehdr hdr;
    read(exe, (char*) &hdr, sizeof(struct ehdr));

    if (hdr.e_machine != RISCV || hdr.e_type != 2) {
        fprintf(stderr, "Bad exe\n");
        return 1;
    }

    pc = (uint32_t*) hdr.e_entry;

    struct phdr* phdrs = calloc(hdr.e_phnum, sizeof(struct phdr));
    read(exe, (char*) phdrs, sizeof(struct phdr) * hdr.e_phnum);

    for (int i = 0; i < hdr.e_phnum; i++) {
        int flags = PROT_READ;
        if (phdrs[i].p_flags == 6) {
            flags |= PROT_WRITE;
        }
        char* pos = (char*) (phdrs[i].p_vaddr - phdrs[i].p_offset);
        uint64_t size = (phdrs[i].p_offset + phdrs[i].p_filesz + 4095) & (-4096);

        if (((uint64_t) pos) + size > heap_start) {
            heap_start = ((uint64_t) pos) + size;
            heap_end = ((uint64_t) pos) + size;
        }

        if (phdrs[i].p_flags == 5) {
            instructions = (uint32_t*) mmap(pos, size, flags, MAP_PRIVATE|MAP_FIXED, exe, 0);
        } else {
            mmap(pos, size, flags, MAP_PRIVATE|MAP_FIXED, exe, 0);
        }
    }
    free(phdrs);

    // stack
    uint64_t STACK_SIZE = 8392704;
    char* stack = mmap((char*) 0x4000000000, STACK_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS, -1, 0);
    // copy argc/argv to stack, skipping emulator args
    uint64_t* sp = (uint64_t*) (stack + STACK_SIZE - (8 * argc + 8));
    sp[0] = (uint64_t) argc;
    for (int i = 0; i < argc; i++) {
        sp[i + 1] = (uint64_t) argv[i];
    }

    if (debug) {
        signal(SIGSEGV, segfault);
    }

    registers[2] = (uint64_t) sp;

    /*
    uint64_t history[200] = {};
    int history_start = 0;
    int history_end = 0;
    */

    while (1) {
        if (debug) {
            prompt();
        }
        uint32_t instruction = pc[0];
        if (debug) {
            print_instruction(instruction);
        }
        perf.instruction_count++;
        pc++;
        uint32_t opcode = instruction & 0b1111111;
        if (opcode == 0b0110011) {
            // R
            uint32_t rd = (instruction >> 7) & 0b11111;
            uint32_t rs1 = (instruction >> 15) & 0b11111;
            uint32_t rs2 = (instruction >> 20) & 0b11111;
            uint32_t funct3 = (instruction >> 12) & 0b111;
            uint32_t funct7 = instruction >> 25;

            uint64_t left = registers[rs1];
            uint64_t right = registers[rs2];
            uint64_t value;
            if (funct3 == 0) {
                if (funct7 == 0x00) {
                    // add
                    value = left + right;
                } else if (funct7 == 0x20) {
                    // sub
                    value = left - right;
                } else if (funct7 == 0x01) {
                    // mul
                    value = left * right;
                }
            } else if (funct3 == 4) {
                if (funct7 == 0x00) {
                    // xor
                    value = left ^ right;
                } else if (funct7 == 0x01) {
                    // div
                    value = left / right;
                }
            } else if (funct3 == 6) {
                if (funct7 == 0x00) {
                    // or
                    value = left | right;
                } else if (funct7 == 0x01) {
                    // rem
                    value = left % right;
                }
            } else if (funct3 == 7) {
                // and
                    value = left & right;
            } else if (funct3 == 1) {
                // sll
                value = left << right;
            } else if (funct3 == 5) {
                if (funct7 == 0x00) {
                    // srl
                    // TODO
                } else if (funct7 == 0x20) {
                    // sra
                    value = left >> right;
                }
            } else if (funct3 == 2) {
                // slt
                // TODO
            } else if (funct3 == 3) {
                // sltu
                // TODO
            }
            if (rd > 0) {
                registers[rd] = value;
            }
        } else if (opcode == 0b0010011) {
            // I
            uint32_t rd = (instruction >> 7) & 0b11111;
            uint32_t rs1 = (instruction >> 15) & 0b11111;
            uint32_t funct3 = (instruction >> 12) & 0b111;
            uint64_t imm = instruction >> 20;
            uint64_t m = 1U << (12 - 1);
            imm = (imm ^ m) - m;

            uint64_t left = registers[rs1];
            uint64_t value;
            if (funct3 == 0) {
                // addi
                value = left + imm;
            } else if (funct3 == 4) {
                // xori
                value = left ^ imm;
            } else if (funct3 == 6) {
                // ori
                value = left | imm;
            } else if (funct3 == 7) {
                // andi
                value = left & imm;
            } else if (funct3 == 1) {
                // slli
                value = left << imm;
            } else if (funct3 == 5) {
                if ((imm >> 5) == 0x20) {
                    // srai
                    value = left >> imm;
                } else {
                    // srli
                    // TODO
                }
            } else if (funct3 == 2) {
                // slti
                // TODO
            } else if (funct3 == 3) {
                // sltiu
                // TODO
            }
            if (rd > 0) {
                registers[rd] = value;
            }
        } else if (opcode == 0b0000011) {
            // I2
            perf.loads++;
            uint32_t rd = (instruction >> 7) & 0b11111;
            uint32_t rs1 = (instruction >> 15) & 0b11111;
            uint32_t funct3 = (instruction >> 12) & 0b111;
            uint64_t imm = instruction >> 20;
            uint64_t m = 1U << (12 - 1);
            imm = (imm ^ m) - m;

            uint64_t loc = registers[rs1] + imm;
            uint64_t value;
            if (funct3 == 0) {
                // lb
                value = ((char*) loc)[0];
                uint64_t m = 1U << (8 - 1);
                value = (value ^ m) - m;
            } else if (funct3 == 1) {
                // lh
                value = ((uint16_t*) loc)[0];
                uint64_t m = 1U << (16 - 1);
                value = (value ^ m) - m;
            } else if (funct3 == 2) {
                // lw
                value = ((uint32_t*) loc)[0];
                uint64_t m = 1U << (32 - 1);
                value = (value ^ m) - m;
            } else if (funct3 == 3) {
                // ld
                value = ((uint64_t*) loc)[0];
            } else if (funct3 == 4) {
                // lbu
                value = ((char*) loc)[0];
            } else if (funct3 == 5) {
                // lhu
                value = ((uint16_t*) loc)[0];
            } else if (funct3 == 6) {
                // lwu
                value = ((uint32_t*) loc)[0];
            }
            if (rd > 0) {
                registers[rd] = value;
            }
        } else if (opcode == 0b0100011) {
            // S
            perf.stores++;
            uint32_t rs1 = (instruction >> 20) & 0b11111;
            uint32_t rd = (instruction >> 15) & 0b11111;
            uint32_t funct3 = (instruction >> 12) & 0b111;
            uint64_t imm = ((instruction >> 20) & 0xfe0) | ((instruction >> 7) & 0x1f);
            uint64_t m = 1U << (12 - 1);
            imm = (imm ^ m) - m;

            uint64_t value = registers[rs1];
            uint64_t loc = registers[rd] + imm;
            if (funct3 == 0) {
                // sb
                char* l = (char*) loc;
                l[0] = (uint8_t) value;
            } else if (funct3 == 1) {
                // sh
                uint16_t* l = (uint16_t*) loc;
                l[0] = (uint16_t) value;
            } else if (funct3 == 2) {
                // sw
                uint32_t* l = (uint32_t*) loc;
                l[0] = (uint32_t) value;
            } else if (funct3 == 3) {
                // sd
                uint64_t* l = (uint64_t*) loc;
                l[0] = value;
            }
        } else if (opcode == 0b1100011) {
            // B
            pc--;
            uint32_t rs1 = (instruction >> 15) & 0b11111;
            uint32_t rs2 = (instruction >> 20) & 0b11111;
            uint32_t funct3 = (instruction >> 12) & 0b111;
            uint64_t imm = ((instruction & 0x80000000) >> 19) | ((instruction & 0x7e000000) >> 20) |
                           ((instruction & 0xf00) >> 7) | ((instruction & 0x80) << 4);
            uint64_t m = 1U << (13 - 1);
            imm = (imm ^ m) - m;
            uint64_t left = registers[rs1];
            uint64_t right = registers[rs2];
            if (funct3 == 0 && left == right) {
                // beq
                pc = (uint32_t*) (((uint64_t) pc) + imm);
            } else if (funct3 == 1 && left != right) {
                // bne
                pc = (uint32_t*) (((uint64_t) pc) + imm);
            } else if (funct3 == 4 && (int64_t)left < (int64_t) right) {
                // blt
                pc = (uint32_t*) (((uint64_t) pc) + imm);
            } else if (funct3 == 5 && (int64_t)left >= (int64_t) right) {
                // bge
                pc = (uint32_t*) (((uint64_t) pc) + imm);
            } else if (funct3 == 6 && left < right) {
                // bltu
                pc = (uint32_t*) (((uint64_t) pc) + imm);
            } else if (funct3 == 7 && left >= right) {
                // bgeu
                pc = (uint32_t*) (((uint64_t) pc) + imm);
            } else {
                pc++;
            }
        } else if (opcode == 0b1101111) {
            // JAL
            uint32_t rd = (instruction >> 7) & 0b11111;
            uint64_t imm = ((instruction & 0x80000000) >> 11) | ((instruction & 0x7fe00000) >> 20) |
                           ((instruction & 0x100000) >> 9) | (instruction & 0xff000);
            uint64_t m = 1U << (21 - 1);
            imm = (imm ^ m) - m;
            if (rd > 0) {
                registers[rd] = (uint64_t) pc;
            }
            pc--;
            pc = (uint32_t*) (((uint64_t) pc) + imm);
        } else if (opcode == 0b1100111) {
            // JALR
            uint32_t rd = (instruction >> 7) & 0b11111;
            uint64_t rs = registers[(instruction >> 15) & 0b11111];
            uint64_t offset = instruction >> 20;
            uint64_t m = 1U << (12 - 1);
            offset = (offset ^ m) - m;
            if (rd > 0) {
                registers[rd] = (uint64_t) pc;
            }
            pc = (uint32_t*) (rs + offset);
        } else if (opcode == 0b0110111) {
            // LUI
            uint64_t imm = (instruction >> 12) << 12;
            uint64_t m = 1U << (32 - 1);
            imm = (imm ^ m) - m;
            uint32_t rd = (instruction >> 7) & 0b11111;
            registers[rd] = imm;
        } else if (opcode == 0b0010111) {
            // AUIPC
            pc--;
            uint64_t imm = (instruction >> 12) << 12;
            uint64_t m = 1U << (32 - 1);
            imm = (imm ^ m) - m;
            imm = imm + (uint64_t) pc;
            uint32_t rd = (instruction >> 7) & 0b11111;
            registers[rd] = imm;
            pc++;
        } else if (opcode == 0b1110011) {
            if ((instruction & (1 << 12)) != 0) {
                // TODO: ebreak
            } else {
                perf.syscalls++;
                if (registers[17] == 214) {
                    // brk
                    if (registers[10] == 0) {
                        registers[10] = heap_end;
                    } else if (registers[10] > heap_end) {
                        uint64_t size = ((registers[10] - heap_end) + 4095) & (-4096);
                        mmap((char*) heap_end, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS, -1, 0);
                        heap_end += size;
                    }
                } else {
                    ecall(registers);
                }
            }
        } else {
            fprintf(stderr, "ILLEGAL INSTRUCTION: %x at %x\n", instruction, pc);
            return 1;
        }
    }
}

void ecall(uint64_t registers[32]) {
    uint16_t syscalls_map[320] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 257, 3, 0, 0, 0, 217, 8, 0,
                                 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 60, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 12, 11, 0, 0, 0, 0, 0, 0, 9, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 332, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};


    register uint64_t r10 asm("r10") = registers[13];
    register uint64_t r8 asm("r8") = registers[14];
    register uint64_t r9 asm("r9") = registers[15];
    uint64_t ret;
    __asm volatile (
        "syscall"
        : "=a"(ret)
        : "a"(syscalls_map[registers[17]]), "D"(registers[10]), "S"(registers[11]), "d"(registers[12]), "r"(r10), "r"(r8), "r"(r9)//, "D"(registers[16])
        : "rcx", "r11", "memory"
    );
    registers[10] = ret;
}

void print_instruction(uint32_t instruction) {
    uint32_t opcode = instruction & 0b1111111;
    if (opcode == 0b0110011) {
        // R
        uint32_t rd = (instruction >> 7) & 0b11111;
        uint32_t rs1 = (instruction >> 15) & 0b11111;
        uint32_t rs2 = (instruction >> 20) & 0b11111;
        uint32_t funct3 = (instruction >> 12) & 0b111;
        uint32_t funct7 = instruction >> 25;

        if (funct3 == 0) {
            if (funct7 == 0x00) {
                // add
                fprintf(stderr, "(add ");
            } else if (funct7 == 0x20) {
                // sub
                fprintf(stderr, "(sub ");
            } else if (funct7 == 0x01) {
                // mul
                fprintf(stderr, "(mul ");
            }
        } else if (funct3 == 4) {
            if (funct7 == 0x00) {
                // xor
                fprintf(stderr, "(xor ");
            } else if (funct7 == 0x01) {
                // div
                fprintf(stderr, "(div ");
            }
        } else if (funct3 == 6) {
            if (funct7 == 0x00) {
                // or
                fprintf(stderr, "(or ");
            } else if (funct7 == 0x01) {
                // rem
                fprintf(stderr, "(rem ");
            }
        } else if (funct3 == 7) {
            // and
            fprintf(stderr, "(and ");
        } else if (funct3 == 1) {
            // sll
            fprintf(stderr, "(sll ");
        } else if (funct3 == 5) {
            if (funct7 == 0x00) {
                // srl
                fprintf(stderr, "(srl ");
            } else if (funct7 == 0x20) {
                // sra
                fprintf(stderr, "(sra ");
            }
        } else if (funct3 == 2) {
            // slt
            fprintf(stderr, "(slt ");
        } else if (funct3 == 3) {
            // sltu
            fprintf(stderr, "(sltu ");
        }
        fprintf(stderr, "x%d x%d x%d)\n", rd, rs1, rs2);
    } else if (opcode == 0b0010011) {
        // I
        uint32_t rd = (instruction >> 7) & 0b11111;
        uint32_t rs1 = (instruction >> 15) & 0b11111;
        uint32_t funct3 = (instruction >> 12) & 0b111;
        uint64_t imm = instruction >> 20;
        uint64_t m = 1U << (12 - 1);
        imm = (imm ^ m) - m;

        if (funct3 == 0) {
            // addi
            fprintf(stderr, "(addi ");
        } else if (funct3 == 4) {
            // xori
            fprintf(stderr, "(xori ");
        } else if (funct3 == 6) {
            // ori
            fprintf(stderr, "(ori ");
        } else if (funct3 == 7) {
            // andi
            fprintf(stderr, "(andi ");
        } else if (funct3 == 1) {
            // slli
            fprintf(stderr, "(slli ");
        } else if (funct3 == 5) {
            if ((imm >> 5) == 0x20) {
                // srai
                fprintf(stderr, "(srai ");
            } else {
                // srli
                fprintf(stderr, "(srli ");
            }
        } else if (funct3 == 2) {
            // slti
            fprintf(stderr, "(slti ");
        } else if (funct3 == 3) {
            // sltiu
            fprintf(stderr, "(sltiu ");
        }
        fprintf(stderr, "x%d x%d %d)\n", rd, rs1, imm);
    } else if (opcode == 0b0000011) {
        // I2
        uint32_t rd = (instruction >> 7) & 0b11111;
        uint32_t rs1 = (instruction >> 15) & 0b11111;
        uint32_t funct3 = (instruction >> 12) & 0b111;
        uint64_t imm = instruction >> 20;
        uint64_t m = 1U << (12 - 1);
        imm = (imm ^ m) - m;

        if (funct3 == 0) {
            // lb
            fprintf(stderr, "(lb ");
        } else if (funct3 == 1) {
            // lh
            fprintf(stderr, "(lh ");
        } else if (funct3 == 2) {
            // lw
            fprintf(stderr, "(lw ");
        } else if (funct3 == 3) {
            // ld
            fprintf(stderr, "(ld ");
        } else if (funct3 == 4) {
            // lbu
            fprintf(stderr, "(lbu ");
        } else if (funct3 == 5) {
            // lhu
            fprintf(stderr, "(lhu ");
        } else if (funct3 == 6) {
            // lwu
            fprintf(stderr, "(lwu ");
        }
        if (imm == 0) {
            fprintf(stderr, "x%d x%d)\n", rd, rs1);
        } else {
            fprintf(stderr, "x%d (+ x%d %d))\n", rd, rs1, imm);
        }
    } else if (opcode == 0b0100011) {
        // S
        uint32_t rs1 = (instruction >> 20) & 0b11111;
        uint32_t rd = (instruction >> 15) & 0b11111;
        uint32_t funct3 = (instruction >> 12) & 0b111;
        uint64_t imm = ((instruction >> 20) & 0xfe0) | ((instruction >> 7) & 0x1f);
        uint64_t m = 1U << (12 - 1);
        imm = (imm ^ m) - m;

        if (funct3 == 0) {
            // sb
            fprintf(stderr, "(sb ");
        } else if (funct3 == 1) {
            // sh
            fprintf(stderr, "(sh ");
        } else if (funct3 == 2) {
            // sw
            fprintf(stderr, "(sw ");
        } else if (funct3 == 3) {
            // sd
            fprintf(stderr, "(sd ");
        }
        if (imm == 0) {
            fprintf(stderr, "x%d x%d)\n", rd, rs1);
        } else {
            fprintf(stderr, "(+ x%d %d) x%d)\n", rd, imm, rs1);
        }
    } else if (opcode == 0b1100011) {
        // B
        uint32_t rs1 = (instruction >> 15) & 0b11111;
        uint32_t rs2 = (instruction >> 20) & 0b11111;
        uint32_t funct3 = (instruction >> 12) & 0b111;
        uint64_t imm = ((instruction & 0x80000000) >> 19) | ((instruction & 0x7e000000) >> 20) |
                       ((instruction & 0xf00) >> 7) | ((instruction & 0x80) << 4);
        uint64_t m = 1U << (13 - 1);
        imm = (imm ^ m) - m;
        if (funct3 == 0) {
            // beq
            fprintf(stderr, "(beq ");
        } else if (funct3 == 1) {
            // bne
            fprintf(stderr, "(bne ");
        } else if (funct3 == 4) {
            // blt
            fprintf(stderr, "(blt ");
        } else if (funct3 == 5) {
            // bge
            fprintf(stderr, "(bge ");
        } else if (funct3 == 6) {
            // bltu
            fprintf(stderr, "(bltu ");
        } else if (funct3 == 7) {
            // bgeu
            fprintf(stderr, "(bgeu ");
        }
        fprintf(stderr, "x%d x%d %d)\n", rs1, rs2, imm);
    } else if (opcode == 0b1101111) {
        // JAL
        uint32_t rd = (instruction >> 7) & 0b11111;
        uint64_t imm = ((instruction & 0x80000000) >> 11) | ((instruction & 0x7fe00000) >> 20) |
                       ((instruction & 0x100000) >> 9) | (instruction & 0xff000);
        uint64_t m = 1U << (21 - 1);
        imm = (imm ^ m) - m;
        fprintf(stderr, "(jal x%d %d)\n", rd, imm);
    } else if (opcode == 0b1100111) {
        // JALR
        uint32_t rd = (instruction >> 7) & 0b11111;
        uint64_t rs = (instruction >> 15) & 0b11111;
        uint64_t offset = instruction >> 20;
        uint64_t m = 1U << (12 - 1);
        offset = (offset ^ m) - m;
        if (offset == 0) {
            fprintf(stderr, "(jalr x%d x%d)\n", rd, rs);
        } else {
            fprintf(stderr, "(jalr x%d (+ x%d %d))\n", rd, rs, offset);
        }
    } else if (opcode == 0b0110111) {
        // LUI
        uint64_t imm = instruction >> 12;
        uint64_t m = 1U << (32 - 1);
        imm = (imm ^ m) - m;
        uint32_t rd = (instruction >> 7) & 0b11111;
        fprintf(stderr, "(lui x%d %d)\n", rd, imm);
    } else if (opcode == 0b0010111) {
        // AUIPC
        uint64_t imm = instruction >> 12;
        uint64_t m = 1U << (32 - 1);
        imm = (imm ^ m) - m;
        uint32_t rd = (instruction >> 7) & 0b11111;
        fprintf(stderr, "(auipc x%d %d)\n", rd, imm);
    } else if (opcode == 0b1110011) {
        if ((instruction & (1 << 12)) != 0) {
            fprintf(stderr, "(ebreak)\n");
        } else {
            fprintf(stderr, "(ecall)\n");
        }
    }
}

void prompt() {
    //print_screen(instructions, registers, pc);
prompt:
    printf("> ");
    fflush(stdout);
    char buf[1024];
dread:
    int size = read(STDIN_FILENO, buf, 1024);
    if (size == 0) {
        goto dread;
    } else if (size == 1) {
        goto prompt;
    }
    size--;

    // add to history
    /*
    char* cmd = malloc(size);
    memcpy(cmd, buf, size);
    history[history_end] = (uint64_t) cmd;
    history[history_end+1] = size;
    history_end = (history_end + 2) % 200;
    if (history_end <= history_start) {
        history_start = (history_start + 2) % 200;
    }
    */

    if (size >= 1 && buf[0] == 's') {
        // step
        return;
    } else if (size == 1 && buf[0] == 'd') {
        fprintf(stderr, "pc:\t0x%x\t%d\t\t", pc, pc);
        fprintf(stderr, "x%d:\t0x%x\t%d\n", 16, registers[16], registers[16]);
        for (int i = 1; i < 16; i++) {
            fprintf(stderr, "x%d:\t0x%x\t%d\t\t", i, registers[i], registers[i]);
            fprintf(stderr, "x%d:\t0x%x\t%d\n", i+16, registers[i+16], registers[i+16]);
        }
        goto prompt;
    } else if (size == 4 && strncmp(buf, "perf", 4) == 0) {
        // print perf
        fprintf(stderr, "Instruction count: %d\n", perf.instruction_count);
        fprintf(stderr, "Loads: %d\n", perf.loads);
        fprintf(stderr, "Stores: %d\n", perf.stores);
        fprintf(stderr, "Syscalls: %d\n", perf.syscalls);
        fprintf(stderr, "Heap size: 0x%x %d\n", heap_end - heap_start, heap_end - heap_start);
        goto prompt;
    } else if (size == 1 && buf[0] == 'i') {
        // print current instruction
        print_instruction((pc - 1)[0]);
        goto prompt;
    } else if (size >= 1 && buf[0] == 'p') {
        // print register
        goto prompt;
    } else if (size == 1 && buf[0] == 'r') {
        // run
        debug = 0;
        return;
    } else if (size >= 1 && buf[0] == 'q') {
        // quit
        // TODO: prompt
        exit(0);
    } else {
        goto prompt;
    }
}
