// EPOS SiFive-U (RISC-V) SETUP

#include <architecture.h>
#include <machine.h>
#include <utility/elf.h>
#include <utility/string.h>

using namespace EPOS::S;
typedef unsigned long Reg;

// timer handler
extern "C" [[gnu::interrupt, gnu::aligned(8)]] void _mmode_forward()
{
    Reg id = CPU::mcause();
    if ((id & CLINT::INT_MASK) == CLINT::IRQ_MAC_TIMER)
    {
        Timer::reset();
        CPU::sie(CPU::STI);
    }
    Reg interrupt_id = 1 << ((id & CLINT::INT_MASK) - 2);
    if (CPU::int_enabled() && (CPU::sie() & (interrupt_id)))
        CPU::mip(interrupt_id);
}

extern "C"
{
    void _start();

    void _int_entry();

    // SETUP entry point is in .init (and not in .text), so it will be linked first and will be the first function after the ELF header in the image
    void _entry() __attribute__((used, naked, section(".init")));
    void _setup();
    void _panic();

    // LD eliminates this variable while performing garbage collection, that's why the used attribute.
    char __boot_time_system_info[sizeof(EPOS::S::System_Info)] __attribute__((used)) = "<System_Info placeholder>"; // actual System_Info will be added by mkbi!
}

__BEGIN_SYS
char * bi;
extern OStream kout, kerr;

class Setup
{
private:
    // Physical memory map
    static const unsigned long RAM_BASE = Memory_Map::RAM_BASE;
    static const unsigned long RAM_TOP = Memory_Map::RAM_TOP;
    static const unsigned long APP_LOW = Memory_Map::APP_LOW;
    static const unsigned long APP_HIGH = Memory_Map::APP_HIGH;
    static const unsigned long APP_CODE = Memory_Map::APP_CODE;
    static const unsigned long APP_DATA = Memory_Map::APP_DATA;
    static const unsigned long MIO_BASE = Memory_Map::MIO_BASE;
    static const unsigned long MIO_TOP = Memory_Map::MIO_TOP;
    static const unsigned long FREE_BASE = Memory_Map::FREE_BASE;
    static const unsigned long FREE_TOP = Memory_Map::FREE_TOP;
    static const unsigned long PHY_MEM = Memory_Map::PHY_MEM;
    static const unsigned long SETUP = Memory_Map::SETUP;
    static const unsigned long BOOT_STACK = Memory_Map::BOOT_STACK;
    static const unsigned long PAGE_TABLES = Memory_Map::PAGE_TABLES;
    static const unsigned long INIT = Memory_Map::INIT;
    // static const unsigned long MMODE_F = Memory_Map::MMODE_F;



    // static const unsigned long SYS = Memory_Map::SYS;
    // static const unsigned long SYS_CODE = Memory_Map::SYS_CODE;
    static const unsigned long SYS_INFO = Memory_Map::SYS_INFO;
    // static const unsigned long SYS_PT = Memory_Map::SYS_PT;
    // static const unsigned long SYS_PD1 = Memory_Map::SYS_PD1;
    // static const unsigned long SYS_PD2 = Memory_Map::SYS_PD2;
    // static const unsigned long SYS_DATA = Memory_Map::SYS_DATA;
    // static const unsigned long SYS_STACK = Memory_Map::SYS_STACK;
    // static const unsigned long SYS_HEAP = Memory_Map::SYS_HEAP;
    // static const unsigned long SYS_HIGH = Memory_Map::SYS_HIGH;



    static const unsigned int PT_ENTRIES = MMU::PT_ENTRIES;
    static const unsigned int PD_ENTRIES = PT_ENTRIES;

    // Architecture Imports
    typedef CPU::Reg Reg;
    typedef CPU::Phy_Addr Phy_Addr;
    typedef CPU::Log_Addr Log_Addr;
    typedef MMU::RV64_Flags RV64_Flags;
    typedef MMU::Page_Table Page_Table;
    typedef MMU::Page_Directory Page_Directory;

public:
    Setup();

private:
    void say_hi();
    void build_lm();
    void load_parts();
    void init_mmu();
    void mmu_init();
    void call_next();

private:
    System_Info *si;
};

Setup::Setup()
{
    Display::init();
    kout << endl;
    kerr << endl;

    si = reinterpret_cast<System_Info *>(&__boot_time_system_info);
    if (si->bm.n_cpus > Traits<Machine>::CPUS)
        si->bm.n_cpus = Traits<Machine>::CPUS;

    db<Setup>(TRC) << "Setup(si=" << reinterpret_cast<void *>(si) << ",sp=" << CPU::sp() << ")" << endl;
    db<Setup>(INF) << "Setup:si=" << *si << endl;

    // Print basic facts about this EPOS instance
    say_hi();

    //build page tables
    init_mmu();

    build_lm();

    load_parts();

    // enable_paging();

    // SETUP ends here, so let's transfer control to the next stage (INIT or APP)
    call_next();
}

void Setup::say_hi()
{
    db<Setup>(TRC) << "Setup::say_hi()" << endl;
    db<Setup>(INF) << "System_Info=" << *si << endl;

    // if (si->bm.application_offset[0] == -1U)
    //     db<Setup>(ERR) << "No APPLICATION in boot image, you don't need EPOS!" << endl;

    kout << "This is EPOS!\n"
         << endl;
    kout << "Setting up this machine as follows: " << endl;
    kout << "  Mode:         " << ((Traits<Build>::MODE == Traits<Build>::LIBRARY) ? "library" : (Traits<Build>::MODE == Traits<Build>::BUILTIN) ? "built-in"
                                                                                                                                                 : "kernel")
         << endl;
    kout << "  Processor:    " << Traits<Machine>::CPUS << " x RV" << Traits<CPU>::WORD_SIZE << " at " << Traits<CPU>::CLOCK / 1000000 << " MHz (BUS clock = " << Traits<CPU>::CLOCK / 1000000 << " MHz)" << endl;
    kout << "  Machine:      SiFive-U" << endl;
    kout << "  Memory:       " << (RAM_TOP + 1 - RAM_BASE) / (1024 * 1024) << " MB [" << reinterpret_cast<void *>(RAM_BASE) << ":" << reinterpret_cast<void *>(RAM_TOP) << "]" << endl;
    kout << "  User memory:  " << (FREE_TOP - FREE_BASE) / (1024 * 1024) << " MB [" << reinterpret_cast<void *>(FREE_BASE) << ":" << reinterpret_cast<void *>(FREE_TOP) << "]" << endl;
    kout << "  I/O space:    " << (MIO_TOP + 1 - MIO_BASE) / (1024 * 1024) << " MB [" << reinterpret_cast<void *>(MIO_BASE) << ":" << reinterpret_cast<void *>(MIO_TOP) << "]" << endl;
    kout << "  Node Id:      ";
    if (si->bm.node_id != -1)
        kout << si->bm.node_id << " (" << Traits<Build>::NODES << ")" << endl;
    else
        kout << "will get from the network!" << endl;
    kout << "  Position:     ";
    if (si->bm.space_x != -1)
        kout << "(" << si->bm.space_x << "," << si->bm.space_y << "," << si->bm.space_z << ")" << endl;
    else
        kout << "will get from the network!" << endl;
    if (si->bm.extras_offset != -1UL)
        kout << "  Extras:       " << si->lm.app_extra_size << " bytes" << endl;

    kout << endl;
}

void Setup::load_parts()
{
    // Relocate System_Info
    if(sizeof(System_Info) > 4096) {
        db<Setup>(ERR) << "System_Info is bigger than a page (" << sizeof(System_Info) << ")!" << endl;
        _panic();
    }
    memcpy(reinterpret_cast<void *>(SYS_INFO), si, sizeof(System_Info));

    // Load INIT
    ELF * ini_elf = reinterpret_cast<ELF *>(&bi[si->bm.init_offset]);
    ELF * sys_elf = reinterpret_cast<ELF *>(&bi[si->bm.system_offset]);

    if(si->lm.has_ini) {
        db<Setup>(WRN) << "Setup_SifiveE::load_init()" << endl;
        if(ini_elf->load_segment(0) < 0) {
            db<Setup>(ERR) << "INIT code segment was corrupted during SETUP!" << endl;
            _panic();
        }

        for(int i = 1; i < ini_elf->segments(); i++)
            if(ini_elf->load_segment(i) < 0) {
                db<Setup>(ERR) << "INIT data segment was corrupted during SETUP!" << endl;
                _panic();
            }
    }

    if((long unsigned int)ini_elf->segment_size(0) > sys_elf->segment_address(0) - ini_elf->segment_address(0)) {
        db<Setup>(ERR) << "init is larger than its reserved memory" << endl;
        _panic();
    }
    db<Setup>(WRN) << "init has " << hex << sys_elf->segment_address(0) - ini_elf->segment_address(0) - ini_elf->segment_size(0) << " unused bytes of memory" << endl;

    // Load SYSTEM
    if(si->lm.has_sys) {
        db<Setup>(WRN) << "Setup_SifiveE::load_system()" << endl;
        if(sys_elf->load_segment(0) < 0) {
            db<Setup>(ERR) << "system code segment was corrupted during SETUP!" << endl;
            _panic();
        }
        for(int i = 1; i < sys_elf->segments(); i++)
            if(sys_elf->load_segment(i) < 0) {
                db<Setup>(ERR) << "system data segment was corrupted during SETUP!" << endl;
                _panic();
            }
    }

    if((long unsigned int)sys_elf->segment_size(0) > sys_elf->segment_address(1) - sys_elf->segment_address(0)) {
        db<Setup>(ERR) << "sys code is larger than its reserved memory" << endl;
        _panic();
    }
    db<Setup>(WRN) << "sys code has " << hex << sys_elf->segment_address(1) - sys_elf->segment_address(0) - sys_elf->segment_size(0) << " unused bytes of memory" << endl;

    if((long unsigned int)ini_elf->segment_size(1) > sys_elf->segment_address(1) + 0x00100000 - sys_elf->segment_address(1)) {
        db<Setup>(ERR) << "init is larger than its reserved memory" << endl;
        _panic();
    }
    db<Setup>(WRN) << "sys data has " << hex << sys_elf->segment_address(1) + 0x00100000 - sys_elf->segment_address(1) - ini_elf->segment_size(1) << " unused bytes of memory" << endl;
}


void Setup::build_lm()
{
    db<Setup>(WRN) << "Build Load Map!" << endl;

    // Get boot image structure
    si->lm.has_stp = (si->bm.setup_offset != -1u);
    si->lm.has_ini = (si->bm.init_offset != -1u);
    si->lm.has_sys = (si->bm.system_offset != -1u);
    si->lm.has_app = (si->bm.application_offset[0] != -1u);
    si->lm.has_ext = (si->bm.extras_offset != -1u);

    // Check SETUP integrity and get the size of its segments
    si->lm.stp_entry = 0;
    si->lm.stp_segments = 0;
    si->lm.stp_code = ~0U;
    si->lm.stp_code_size = 0;
    si->lm.stp_data = ~0U;
    si->lm.stp_data_size = 0;

    db<Setup>(WRN) << "Before Boot Info!" << endl;


    bi = reinterpret_cast<char *>(Traits<Machine>::RAM_BASE); // bi is loaded at MEM_BASE
    if(si->lm.has_stp) {
      db<Setup>(WRN) << "Before ELF Cast!" << endl;
        ELF * stp_elf = reinterpret_cast<ELF *>(&bi[si->bm.setup_offset]);
        db<Setup>(WRN) << "After ELF Cast!" << endl;
        if(!stp_elf->valid()) {
            db<Setup>(WRN) << "Inside Valid!" << endl;
            db<Setup>(ERR) << "SETUP ELF image is corrupted!" << endl;
            _panic();
        }

        db<Setup>(WRN) << "Before Setup Elf!" << endl;

        si->lm.stp_entry = stp_elf->entry();
        si->lm.stp_segments = stp_elf->segments();
        si->lm.stp_code = stp_elf->segment_address(0);
        si->lm.stp_code_size = stp_elf->segment_size(0);
        if(stp_elf->segments() > 1) {
            for(int i = 1; i < stp_elf->segments(); i++) {
                if(stp_elf->segment_type(i) != PT_LOAD)
                    continue;
                if(stp_elf->segment_address(i) < si->lm.stp_data)
                    si->lm.stp_data = stp_elf->segment_address(i);
                si->lm.stp_data_size += stp_elf->segment_size(i);
            }
        }
    }
    db<Setup>(WRN) << "After Boot Info!" << endl;

    // Check INIT integrity and get the size of its segments
    si->lm.ini_entry = 0;
    si->lm.ini_segments = 0;
    si->lm.ini_code = ~0U;
    si->lm.ini_code_size = 0;
    si->lm.ini_data = ~0U;
    si->lm.ini_data_size = 0;
    if(si->lm.has_ini) {
        ELF * ini_elf = reinterpret_cast<ELF *>(&bi[si->bm.init_offset]);
        if(!ini_elf->valid()) {
            db<Setup>(ERR) << "INIT ELF image is corrupted!" << endl;
            _panic();
        }

        si->lm.ini_entry = ini_elf->entry();
        si->lm.ini_segments = ini_elf->segments();
        si->lm.ini_code = ini_elf->segment_address(0);
        si->lm.ini_code_size = ini_elf->segment_size(0);
        if(ini_elf->segments() > 1) {
            for(int i = 1; i < ini_elf->segments(); i++) {
                if(ini_elf->segment_type(i) != PT_LOAD)
                    continue;
                if(ini_elf->segment_address(i) < si->lm.ini_data)
                    si->lm.ini_data = ini_elf->segment_address(i);
                si->lm.ini_data_size += ini_elf->segment_size(i);
            }
        }
    }
    db<Setup>(ERR) << "Check Segment integrity!" << endl;

    // Check SYSTEM integrity and get the size of its segments
    si->lm.sys_entry = 0;
    si->lm.sys_segments = 0;
    si->lm.sys_code = ~0U;
    si->lm.sys_code_size = 0;
    si->lm.sys_data = ~0U;
    si->lm.sys_data_size = 0;
    // si->lm.sys_stack = SYS_STACK;
    // si->lm.sys_stack_size = Traits<System>::STACK_SIZE * si->bm.n_cpus;
    if(si->lm.has_sys) {
        ELF * sys_elf = reinterpret_cast<ELF *>(&bi[si->bm.system_offset]);
        if(!sys_elf->valid()) {
            db<Setup>(ERR) << "OS ELF image is corrupted!" << endl;
            _panic();
        }

        si->lm.sys_entry = sys_elf->entry();
        si->lm.sys_segments = sys_elf->segments();
        si->lm.sys_code = sys_elf->segment_address(0);
        si->lm.sys_code_size = sys_elf->segment_size(0);
        if(sys_elf->segments() > 1) {
            for(int i = 1; i < sys_elf->segments(); i++) {
                if(sys_elf->segment_type(i) != PT_LOAD)
                    continue;
                if(sys_elf->segment_address(i) < si->lm.sys_data)
                    si->lm.sys_data = sys_elf->segment_address(i);
                si->lm.sys_data_size += sys_elf->segment_size(i);
            }
        }
    }
    db<Setup>(ERR) << "Check Application integrity!" << endl;

    // Check APPLICATION integrity and get the size of its segments
    for(unsigned i=0; i < si->bm.n_apps; i++){
        si->lm.app[i].app_entry = 0;
        si->lm.app[i].app_segments = 0;
        si->lm.app[i].app_code = ~0U;
        si->lm.app[i].app_code_size = 0;
        si->lm.app[i].app_data = ~0U;
        si->lm.app[i].app_data_size = 0;
        if(si->lm.has_app) {
            ELF * app_elf = reinterpret_cast<ELF *>(&bi[si->bm.application_offset[i]]);
            if(!app_elf->valid()) {
                db<Setup>(ERR) << "Application ELF image is corrupted!" << endl;
                _panic();
            }
            si->lm.app[i].app_entry = app_elf->entry();
            si->lm.app[i].app_segments = app_elf->segments();
            si->lm.app[i].app_code = app_elf->segment_address(0);
            si->lm.app[i].app_code_size = app_elf->segment_size(0);
            if(app_elf->segments() > 1) {
                for(int i = 1; i < app_elf->segments(); i++) {
                    if(app_elf->segment_type(i) != PT_LOAD)
                        continue;
                    if(app_elf->segment_address(i) < si->lm.app[i].app_data)
                        si->lm.app[i].app_data = app_elf->segment_address(i);
                    si->lm.app[i].app_data_size += app_elf->segment_size(i);
                }
            }
        }
    }
    db<Setup>(WRN) << "Finished Building Map!" << endl;

}

void Setup::init_mmu()
{

    unsigned int PAGE_SIZE = 4 * 1024;
    unsigned int PT_ENTRIES = MMU::PT_ENTRIES;
    unsigned long pages = MMU::pages(RAM_TOP + 1);

    kout << "Total Pages: " << pages << endl;

    unsigned total_pts = MMU::page_tables(pages);
    kout << "Total Page Tables: " << total_pts << endl;


    unsigned int PD_ENTRIES_LVL_2 = total_pts / PT_ENTRIES;
    unsigned int PD_ENTRIES_LVL_1 = PT_ENTRIES;
    unsigned int PT_ENTRIES_LVL_0 = PT_ENTRIES;

    Phy_Addr PD2_ADDR = PAGE_TABLES;
    Page_Directory * master = new ((void *)PD2_ADDR) Page_Directory();
    kout << "Master Base Address: " << PD2_ADDR << endl;
    PD2_ADDR += PAGE_SIZE;

    master->remap(PD2_ADDR, RV64_Flags::V, 0, PD_ENTRIES_LVL_2);

    Phy_Addr PD1_ADDR = PD2_ADDR + PT_ENTRIES * PAGE_SIZE;
    Phy_Addr PT0_ADDR = PD1_ADDR;

    for (unsigned long i = 0; i < PD_ENTRIES_LVL_2; i++)
    {
        Page_Directory *pd_lv1 = new ((void *)PD2_ADDR) Page_Directory();
        PD2_ADDR += PAGE_SIZE;

        pd_lv1->remap(PD1_ADDR, RV64_Flags::V, 0, PD_ENTRIES_LVL_1);
        PD1_ADDR += PD_ENTRIES_LVL_1 * PAGE_SIZE;
    }

    PD1_ADDR = 0;
    for (unsigned long i = 0; i < PD_ENTRIES_LVL_2; i++)
    {
        for (unsigned long j = 0; j < PD_ENTRIES_LVL_1; j++)
        {
            Page_Table *pt_lv0 = new ((void *)PT0_ADDR) Page_Table();
            PT0_ADDR += PAGE_SIZE;
            pt_lv0->remap(PD1_ADDR, RV64_Flags::SYS, 0, PT_ENTRIES_LVL_0);
            PD1_ADDR += PD_ENTRIES_LVL_1 * PAGE_SIZE;
        }
    }
    kout << "Page Directory LVL1 Address" << PD1_ADDR << endl;

    kout << "Page Directory End Address" << PD2_ADDR << endl;

    db<Setup>(WRN) << "Set SATP" << endl;
    // Set SATP and enable paging
    CPU::satp((1UL << 63) | (reinterpret_cast<unsigned long>(master) >> 12));

    db<Setup>(WRN) << "Flush TLB" << endl;
    // Flush TLB to ensure we've got the right memory organization
    MMU::flush_tlb();
}

void Setup::call_next()
{
    db<Setup>(WRN) << "SETUP almost ready!" << endl;

    CPU::sie(CPU::SSI | CPU::STI | CPU::SEI);
    CPU::sstatus(CPU::SPP_S);

    CPU::sepc(si->lm.ini_entry);
    CLINT::stvec(CLINT::DIRECT, CPU::Reg(&_int_entry));

    CPU::sret();
    db<Setup>(ERR) << "OS failed to init!" << endl;
}

__END_SYS

using namespace EPOS::S;

void _entry() // machine mode
{
    db<Setup>(WRN) << "Entrou no Entry" << endl;
    // SiFive-U core 0 doesn't have MMU
    if (CPU::mhartid() == 0)
        CPU::halt();

    // ensure that sapt is 0
    db<Setup>(WRN) << "SATP" << endl;

    CPU::satp(0);
    Machine::clear_bss();
    db<Setup>(WRN) << "Stack Pointer" << endl;

    // need to check?
    // set the stack pointer, thus creating a stack for SETUP
    CPU::sp(Memory_Map::BOOT_STACK - Traits<Machine>::STACK_SIZE);

    // Set up the Physical Memory Protection registers correctly
    // A = NAPOT, X, R, W
    CPU::pmpcfg0(0x1f);
    // All memory
    CPU::pmpaddr0((1UL << 55) - 1);

    // Delegate all traps to supervisor
    // Timer will not be delegated due to architecture reasons.
    CPU::mideleg(CPU::SSI | CPU::STI | CPU::SEI);
    CPU::medeleg(0xffff);

    // Relocate _mmode_forward - 1024 bytes are enough
    char * src = reinterpret_cast<char *>(&_mmode_forward);
    char * dst = reinterpret_cast<char *>(Memory_Map::MMODE_F);
    for(int i=0; i < 1024; i++){
        *dst = *src;
        src++;
        dst++;
    }

    CPU::mies(CPU::MSI | CPU::MTI | CPU::MEI);              // enable interrupts generation by CLINT
    CPU::mint_disable();                                    // (mstatus) disable interrupts (they will be reenabled at Init_End)
    CLINT::mtvec(CLINT::DIRECT, CPU::Reg(Memory_Map::MMODE_F)); // setup a preliminary machine mode interrupt handler pointing it to _mmode_forward

    db<Setup>(WRN) << "Setup" << endl;

    // MPP_S = change to supervirsor
    // MPIE = otherwise we won't ever receive interrupts
    CPU::mstatus(CPU::MPP_S | CPU::MPIE);
    CPU::mepc(CPU::Reg(&_setup)); // entry = _setup
    CPU::mret();                  // enter supervisor mode at setup (mepc) with interrupts enabled (mstatus.mpie = true)
}

void _setup() // supervisor mode
{
    db<Setup>(WRN) << "Entrou no Setup" << endl;

    kerr << endl;
    kout << endl;

    Setup setup;
}
