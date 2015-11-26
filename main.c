/* UniDOS emulator */
/* By Nguyen Anh Quynh, 2015 */

#include <unicorn/unicorn.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include <stddef.h> // offsetof()

#include "int20.h"
#include "int21.h"

#define DOS_ADDR 0x100
#define MEM_SIZE 1 << 20 // 1 MB


#pragma pack(push, 1)
struct PSP {
    uint8_t  CPMExit[2];    // 0
    uint16_t FirstFreeSegment;  // 2
    uint8_t  Reserved1; // 4
    uint8_t  CPMCall5Compat[5]; // 5
    uint32_t OldTSRAddress; // 12
    uint32_t OldBreakAddress;
    uint32_t CriticalErrorHandlerAddress;
    uint16_t CallerPSPSegment;
    uint8_t  JobFileTable[20];
    uint16_t EnvironmentSegment;
    uint32_t INT21SSSP;
    uint16_t JobFileTableSize;
    uint32_t JobFileTablePointer;
    uint32_t PreviousPSP;
    uint32_t Reserved2;
    uint16_t DOSVersion;
    uint8_t  Reserved3[14];
    uint8_t  DOSFarCall[3];
    uint16_t Reserved4;
    uint8_t  ExtendedFCB1[7];
    uint8_t  FCB1[16];
    uint8_t  FCB2[20];
    uint8_t  CommandLineLength;
    char     CommandLine[127];
};
#pragma pack(pop)


static void usage(const char *prog)
{
    printf("UniDOS for DOS emulation. Based on Unicorn engine (www.unicorn-engine.org)\n");
    printf("Syntax: %s <COM>\n", prog);
}

static void setup_psp(int16_t seg, uint8_t *memory, int argc, char **argv)
{
    uint32_t abs = MK_FP(seg, 0);
    int j;
    uint8_t c = 0;
    struct PSP *PSP = (struct PSP *)(memory + abs);

    // CPMExit: INT 20h
    PSP->CPMExit[0] = 0xcd;
    PSP->CPMExit[1] = 0x20;

    // DOS Far Call: INT 21h + RETF
    PSP->DOSFarCall[0] = 0xcd;
    PSP->DOSFarCall[1] = 0x21;
    PSP->DOSFarCall[2] = 0xcb;

    // first FSB = empty file name
    PSP->FCB1[0] = 0x01;
    PSP->FCB1[1] = 0x20;

    for (int i = 2; i < argc && c < 0x7E; i++) {
        j = 0;
        PSP->CommandLine[c++] = ' ';
        while (argv[i][j] && c < 0x7E)
            PSP->CommandLine[c++] = argv[i][j++];
    }

    PSP->CommandLine[c] = 0x0D;
    PSP->CommandLineLength = c;
    /*
    printf("==== offset of c = %x\n", offsetof(struct PSP, CommandLineLength));
    printf("==== offset of c1 = %x\n", offsetof(struct PSP, CPMCall5Compat));
    printf("==== offset of c1 = %u\n", offsetof(struct PSP, OldTSRAddress));
    printf("==== cmd[1] = %c\n", PSP->CommandLine[1]);
    printf("==== cmd[2] = %c\n", PSP->CommandLine[2]);
    */
}

static size_t load_com(uc_engine *uc, uint8_t *memory, const char *fname)
{
    FILE *f = fopen(fname, "rb");
    if (f == NULL) {
        perror("Error");
        uc_close(uc);
        exit(EXIT_FAILURE);
    }

    // find the file size
    fseek(f, 0, SEEK_END);      // seek to end of file
    size_t fsize = ftell(f);    // get current file pointer
    fseek(f, 0, SEEK_SET);      // seek back to beginning of file

    // check .com size 
    if (fsize == 0 || fsize > 0xff00) {
        fprintf(stderr, "Invalid file size\n");
        fclose(f);
        uc_close(uc);
        exit(EXIT_FAILURE);
    }

    // copy data in from 0x100
    memset(memory, 0, MEM_SIZE);
    fread(memory + DOS_ADDR, fsize, 1, f);

    // initialize stack pointer
    uint16_t r_sp = 0xfffe;
    uc_reg_write(uc, UC_X86_REG_SP, &r_sp);

    // initialize segment registers
    uint16_t r_cs = 0,
             r_ds = 0,
             r_es = 0,
             r_ss = 0;

    uc_reg_write(uc, UC_X86_REG_CS, &r_cs);
    uc_reg_write(uc, UC_X86_REG_DS, &r_ds);
    uc_reg_write(uc, UC_X86_REG_ES, &r_es);
    uc_reg_write(uc, UC_X86_REG_SS, &r_ss);

    fclose(f);

    return fsize;
}

// callback for handling interrupt
void hook_intr(uc_engine *uc, uint32_t intno, void *user_data)
{
    uint32_t r_ip;
    uint8_t r_ah;

    uc_reg_read(uc, UC_X86_REG_IP, &r_ip);
    uc_reg_read(uc, UC_X86_REG_AH, &r_ah);

    // only handle DOS interrupt
    switch(intno) {
        default:
            printf(">>> 0x%x: interrupt: %x, function %x\n", r_ip, intno, r_ah);
            break;
        case 0x21:
            int21(uc);
            break;
        case 0x20:
            int20(uc);
            break;
    }
}

int main(int argc, char **argv)
{
    uc_engine *uc;
    uc_hook trace;
    uc_err err;

    uint8_t memory[MEM_SIZE];

    if (argc == 1) {
        usage(argv[0]);
        return -1;
    }

    const char *fname = argv[1];

    err = uc_open (UC_ARCH_X86, UC_MODE_16, &uc);
    if (err) {
        fprintf(stderr, "Cannot initialize unicorn\n");
        return 1;
    }

    // map 64KB in
    if (uc_mem_map (uc, 0, MEM_SIZE, UC_PROT_ALL)) {
        fprintf(stderr, "Failed to write emulation code to memory, quit!\n");
        uc_close(uc);
        return 0;
    }

    // initialize internal settings
    int21_init();

    //load executable
    size_t fsize = load_com(uc, memory, fname);

    // setup PSP
    setup_psp(0, memory, argc, argv);

    // write machine code to be emulated in, including the prefix PSP
    uc_mem_write(uc, 0, memory, DOS_ADDR + fsize);

    // handle interrupt ourself
    uc_hook_add(uc, &trace, UC_HOOK_INTR, hook_intr, NULL);

    err = uc_emu_start(uc, DOS_ADDR, DOS_ADDR + 0x10000, 0, 0);
    if (err) {
        fprintf(stderr, "Failed on uc_emu_start() with error returned %u: %s\n",
                err, uc_strerror(err));
    }

    uc_close(uc);

    return 0;
}
