// EPOS SiFive-U (RISC-V) SETUP

#include <architecture.h>
#include <machine.h>
#include <architecture/rv64/rv64_mmu.h>
#include <utility/elf.h>
#include <utility/string.h>


using namespace EPOS::S;
typedef unsigned long Reg;


__BEGIN_SYS

extern "C"
{
    void _start();

    void _int_entry();

    // SETUP entry point is in .init (and not in .text), so it will be linked first and will be the first function after the ELF header in the image
    void _entry() __attribute__((used, naked, section(".init")));
    void _setup();

    // LD eliminates this variable while performing garbage collection, that's why the used attribute.
    char __boot_time_system_info[sizeof(EPOS::S::System_Info)] __attribute__((used)) = "<System_Info placeholder>"; // actual System_Info will be added by mkbi!
}

extern "C" [[gnu::interrupt, gnu::aligned(8)]] void _mmode_forward()
{
    Reg id = CPU::mcause();
    if ((id & IC::INT_MASK) == CLINT::IRQ_MAC_TIMER)
    {
        Timer::reset();
        CPU::sie(CPU::STI);
    }
    Reg interrupt_id = 1 << ((id & IC::INT_MASK) - 2);
    if (CPU::int_enabled() && (CPU::sie() & (interrupt_id)))
        CPU::mip(interrupt_id);
}

extern OStream kout, kerr;

class Setup
{
private:
    // Physical memory map
    static const unsigned long RAM_BASE = Memory_Map::RAM_BASE;
    static const unsigned long RAM_TOP = Memory_Map::RAM_TOP;
    static const unsigned long MIO_BASE = Memory_Map::MIO_BASE;
    static const unsigned long MIO_TOP = Memory_Map::MIO_TOP;
    static const unsigned long FREE_BASE = Memory_Map::FREE_BASE;
    static const unsigned long FREE_TOP = Memory_Map::FREE_TOP;
    static const unsigned long SETUP = Memory_Map::SETUP;
    static const unsigned long BOOT_STACK = Memory_Map::BOOT_STACK;

    // Architecture Imports
    typedef CPU::Reg Reg;
    typedef CPU::Phy_Addr Phy_Addr;
    typedef CPU::Log_Addr Log_Addr;
    typedef MMU::Page Page;
    typedef MMU::RV64_Flags Flags;
    typedef MMU::Page_Table Page_Table;
    typedef MMU::Page_Directory Page_Directory;
    typedef MMU::PT_Entry PT_Entry;
    typedef MMU::PD_Entry PD_Entry;

public:
    Setup();

private:
    void say_hi();
    void call_next();
    void init_mmu();
    void build_lm();
    void build_pmm();

    void panic() { Machine::panic(); }

private:
    System_Info *si;
    char * bi;
};

Setup::Setup()
{
    CPU::int_disable();
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

    init_mmu();

    //EnablePaging -> activate
    //MMU::Directory::activate();
    MMU::flush_tlb();


    // SETUP ends here, so let's transfer control to the next stage (INIT or APP)
    call_next();
}

void Setup::build_lm()
{
    db<Setup>(TRC) << "Setup::build_lm()" << endl;

    // Get boot image structure
    si->lm.has_stp = (si->bm.setup_offset != -1u);
    si->lm.has_ini = (si->bm.init_offset != -1u);
    si->lm.has_sys = (si->bm.system_offset != -1u);
    si->lm.has_app = (si->bm.application_offset != -1u);
    si->lm.has_ext = (si->bm.extras_offset != -1u);

    // Check SETUP integrity and get the size of its segments
    si->lm.stp_entry = 0;
    si->lm.stp_segments = 0;
    si->lm.stp_code = ~0U;
    si->lm.stp_code_size = 0;
    si->lm.stp_data = ~0U;
    si->lm.stp_data_size = 0;
    if(si->lm.has_stp) {
        ELF * stp_elf = reinterpret_cast<ELF *>(&bi[si->bm.setup_offset]);
        if(!stp_elf->valid()) {
            db<Setup>(ERR) << "SETUP ELF image is corrupted!" << endl;
            panic();
        }

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
            panic();
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

    // Check SYSTEM integrity and get the size of its segments
    si->lm.sys_entry = 0;
    si->lm.sys_segments = 0;
    si->lm.sys_code = ~0U;
    si->lm.sys_code_size = 0;
    si->lm.sys_data = ~0U;
    si->lm.sys_data_size = 0;
    si->lm.sys_stack = Memory_Map::SYS_STACK;
    si->lm.sys_stack_size = Traits<System>::STACK_SIZE * si->bm.n_cpus;
    if(si->lm.has_sys) {
        ELF * sys_elf = reinterpret_cast<ELF *>(&bi[si->bm.system_offset]);
        if(!sys_elf->valid()) {
            db<Setup>(ERR) << "OS ELF image is corrupted!" << endl;
            panic();
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

        if(si->lm.sys_code != Memory_Map::SYS_CODE) {
            db<Setup>(ERR) << "OS code segment address (" << reinterpret_cast<void *>(si->lm.sys_code) << ") does not match the machine's memory map (" << reinterpret_cast<void *>(Memory_Map::SYS_CODE) << ")!" << endl;
            panic();
        }
        if(si->lm.sys_code + si->lm.sys_code_size > si->lm.sys_data) {
            db<Setup>(ERR) << "OS code segment is too large!" << endl;
            panic();
        }
        if(si->lm.sys_data != Memory_Map::SYS_DATA) {
            db<Setup>(ERR) << "OS data segment address (" << reinterpret_cast<void *>(si->lm.sys_data) << ") does not match the machine's memory map (" << reinterpret_cast<void *>(Memory_Map::SYS_DATA) << ")!" << endl;
            panic();
        }
        if(si->lm.sys_data + si->lm.sys_data_size > si->lm.sys_stack) {
            db<Setup>(ERR) << "OS data segment is too large!" << endl;
            panic();
        }
        if(MMU::page_tables(MMU::pages(si->lm.sys_stack - Memory_Map::SYS + si->lm.sys_stack_size)) > 1) {
            db<Setup>(ERR) << "OS stack segment is too large!" << endl;
            panic();
        }
    }

    // Check APPLICATION integrity and get the size of its segments
    si->lm.app_entry = 0;
    si->lm.app_segments = 0;
    si->lm.app_code = ~0U;
    si->lm.app_code_size = 0;
    si->lm.app_data = ~0U;
    si->lm.app_data_size = 0;
    if(si->lm.has_app) {
        ELF * app_elf = reinterpret_cast<ELF *>(&bi[si->bm.application_offset]);
        if(!app_elf->valid()) {
            db<Setup>(ERR) << "APP ELF image is corrupted!" << endl;
            panic();
        }
        si->lm.app_entry = app_elf->entry();
        si->lm.app_segments = app_elf->segments();
        si->lm.app_code = app_elf->segment_address(0);
        si->lm.app_code_size = app_elf->segment_size(0);
        if(app_elf->segments() > 1) {
            for(int i = 1; i < app_elf->segments(); i++) {
                if(app_elf->segment_type(i) != PT_LOAD)
                    continue;
                if(app_elf->segment_address(i) < si->lm.app_data)
                    si->lm.app_data = app_elf->segment_address(i);
                si->lm.app_data_size += app_elf->segment_size(i);
            }
        }
        if(si->lm.app_data == ~0U) {
            db<Setup>(WRN) << "APP ELF image has no data segment!" << endl;
            si->lm.app_data = MMU::align_page(Memory_Map::APP_DATA);
        }
        if(Traits<System>::multiheap) { // Application heap in data segment
            si->lm.app_data_size = MMU::align_page(si->lm.app_data_size);
            si->lm.app_stack = si->lm.app_data + si->lm.app_data_size;
            si->lm.app_data_size += MMU::align_page(Traits<Application>::STACK_SIZE);
            si->lm.app_heap = si->lm.app_data + si->lm.app_data_size;
            si->lm.app_data_size += MMU::align_page(Traits<Application>::HEAP_SIZE);
        }
        if(si->lm.has_ext) { // Check for EXTRA data in the boot image
            si->lm.app_extra = si->lm.app_data + si->lm.app_data_size;
            si->lm.app_extra_size = si->bm.img_size - si->bm.extras_offset;
            if(Traits<System>::multiheap)
                si->lm.app_extra_size = MMU::align_page(si->lm.app_extra_size);
            si->lm.app_data_size += si->lm.app_extra_size;
        }
    }
}

void Setup::build_pmm()
{
    db<Setup>(TRC) << "Setup::build_pmm()" << endl;

    //Número de paginas no sistema e a pagina atual (de cima pra baixo)
    //Sempre retorna pelo menos uma página
    Phy_Addr top_page = MMU::pages(si->bm.mem_top - si->bm.mem_base); // deveria ter ( - si->bm.mem_base )

    // Machine to Supervisor code (1 x sizeof(Page), not listed in the PMM)
    top_page -= 1;

    // System Info (1 x sizeof(Page))
    top_page -= 1;
    si->pmm.sys_info = top_page * sizeof(Page);

    // System Page Table (1 x sizeof(Page))
    top_page -= 1;
    si->pmm.sys_pt = top_page * sizeof(Page);

    // System Page Directory 1 (1 x sizeof(Page))
    top_page -= 2;
    si->pmm.sys_pd = top_page * sizeof(Page);

    // SYSTEM code segment
    top_page -= MMU::pages(si->lm.sys_code_size);
    si->pmm.sys_code = top_page * sizeof(Page);

    // SYSTEM data segment
    top_page -= MMU::pages(si->lm.sys_data_size);
    si->pmm.sys_data = top_page * sizeof(Page);

    // SYSTEM stack segment
    top_page -= MMU::pages(si->lm.sys_stack_size);
    si->pmm.sys_stack = top_page * sizeof(Page);


    // Page tables to map the whole physical memory
    // = NP/NPTE_PT * sizeof(Page)
    //   NP = size of physical memory in pages
    //   NPTE_PT = number of page table entries per page table
    //(0x88000000 - 0x8000000) / 4 * 1024 = (0x8000 + 511)/512 = 0x40 = 64
    //64 * 512 * 4 * 1024 = 128MB   -> 64 PD & 512 PT


    //Dividir o PD em 2 níveis. Dividir por 2 e ter sempre 2 tables no 1º nível.



    //16GB:  0x400000000 -> %4KiB = 0x400000 -> %512 = 0x2000 = 8192 / 512

    unsigned int n_pds = MMU::page_tables(MMU::pages(si->bm.mem_top - si->bm.mem_base));
    unsigned int n_pd1 = n_pds;
    unsigned int n_pd2 = 1;
    if (n_pds > MMU::PD_ENTRIES) {
        n_pd2 = n_pd1 / MMU::PD_ENTRIES;
        n_pd1 /= n_pd2;
    }
    top_page -= n_pd1;
    si->pmm.phy_mem_pts = top_page * sizeof(Page);

    top_page -= n_pd2;
    si->pmm.phy_mem_pts = top_page * sizeof(Page);

    // Page tables to map the IO address space
    // = NP/NPTE_PT * sizeof(Page)
    // NP = size of I/O address space in pages
    // NPTE_PT = number of page table entries per page table
    top_page -= MMU::page_tables(MMU::pages(si->bm.mio_top - si->bm.mio_base));
    si->pmm.io_pts = top_page * sizeof(Page);

    // The memory allocated so far will "disappear" from the system as we set mem_top as follows:
    si->pmm.usr_mem_base = si->bm.mem_base;
    si->pmm.usr_mem_top = top_page * sizeof(Page);

    // Free chunks (passed to MMU::init)
    si->pmm.free1_base = si->lm.has_ext ? si->lm.app_extra + si->lm.app_extra_size : si->lm.app_data + si->lm.app_data_size;
    si->pmm.free1_top = top_page * sizeof(Page);

    // Test if we didn't overlap SETUP and the boot image
    if(si->pmm.usr_mem_top <= si->lm.stp_code + si->lm.stp_code_size + si->lm.stp_data_size) {
        db<Setup>(ERR) << "SETUP would have been overwritten!" << endl;
        panic();
    }
}

void Setup::say_hi()
{
    db<Setup>(TRC) << "Setup::say_hi()" << endl;
    db<Setup>(INF) << "System_Info=" << *si << endl;

    if (si->bm.application_offset == -1U)
        db<Setup>(ERR) << "No APPLICATION in boot image, you don't need EPOS!" << endl;

    kout << "This is EPOS!\n"
         << endl;
    kout << "Setting up this machine as follows: " << endl;
    kout << "  Mode:         " << ((Traits<Build>::MODE == Traits<Build>::LIBRARY) ? "library" : (Traits<Build>::MODE == Traits<Build>::BUILTIN) ? "built-in"
                                                                                                                                                 : "kernel")
         << endl;
    kout << "  Processor:    " << Traits<Machine>::CPUS << " x RV" << Traits<CPU>::WORD_SIZE << " at " << Traits<CPU>::CLOCK / 1000000 << " MHz (BUS clock = " << Traits<CPU>::CLOCK / 1000000 << " MHz)" << endl;
    kout << "  Machine:      SiFive-U" << endl;
    kout << "  Memory:       " << (RAM_TOP + 1 - RAM_BASE) / 1024 << " KB [" << reinterpret_cast<void *>(RAM_BASE) << ":" << reinterpret_cast<void *>(RAM_TOP) << "]" << endl;
    kout << "  User memory:  " << (FREE_TOP - FREE_BASE) / 1024 << " KB [" << reinterpret_cast<void *>(FREE_BASE) << ":" << reinterpret_cast<void *>(FREE_TOP) << "]" << endl;
    kout << "  I/O space:    " << (MIO_TOP + 1 - MIO_BASE) / 1024 << " KB [" << reinterpret_cast<void *>(MIO_BASE) << ":" << reinterpret_cast<void *>(MIO_TOP) << "]" << endl;
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

void Setup::init_mmu(){

     PT_Entry * sys_pt = reinterpret_cast<PT_Entry *>(si->pmm.sys_pt);
     memset(sys_pt, 0, sizeof(Page));

     sys_pt[MMU::page(Memory_Map::SYS_INFO)] = MMU::phy2pte(si->pmm.sys_info, Flags::SYS);
     sys_pt[MMU::page(Memory_Map::SYS_PT)] = MMU::phy2pte(si->pmm.sys_pt, Flags::SYS);
     sys_pt[MMU::page(Memory_Map::SYS_PD1)] = MMU::phy2pte(si->pmm.sys_pd, Flags::SYS);

     //sys_pt[MMU::page(Memory_Map::SYS_PD2)] = MMU::phy2pte(si->pmm.sys_pd, Flags::SYS);

     unsigned int i;
    //  unsigned int j;
     PT_Entry aux;

     for(i = 0, aux = si->pmm.sys_code; i < MMU::pages(si->lm.sys_code_size); i++, aux = aux + sizeof(Page))
         sys_pt[MMU::page(Memory_Map::SYS_CODE) + i] = MMU::phy2pte(aux, Flags::SYS);

     for(i = 0, aux = si->pmm.sys_data; i < MMU::pages(si->lm.sys_data_size); i++, aux = aux + sizeof(Page))
         sys_pt[MMU::page(Memory_Map::SYS_DATA) + i] = MMU::phy2pte(aux, Flags::SYS);

     for(i = 0, aux = si->pmm.sys_stack; i < MMU::pages(si->lm.sys_stack_size); i++, aux = aux + sizeof(Page))
         sys_pt[MMU::page(Memory_Map::SYS_STACK) + i] = MMU::phy2pte(aux, Flags::SYS);

     db<Setup>(INF) << "SYS_PT=" << *reinterpret_cast<Page_Table *>(sys_pt) << endl;


    PD_Entry * sys_pd = reinterpret_cast<PD_Entry *>(si->pmm.sys_pd);

    memset(sys_pd, 0, sizeof(Page));

    unsigned int mem_size = MMU::pages(si->bm.mem_top - si->bm.mem_base);
    int n_pts = MMU::page_tables(mem_size);

    PT_Entry * pts = reinterpret_cast<PT_Entry *>(si->pmm.phy_mem_pts);
    for(unsigned int i = 0; i < mem_size; i++)
        pts[i] = MMU::phy2pte((si->bm.mem_base + i * sizeof(Page)), Flags::SYS);


    //assert((MMU::directory(MMU::align_directory(MEM_BASE)) + n_pts) < (MMU::PD_ENTRIES - 4)); // check if it would overwrite the OS
    for(unsigned int i = MMU::directory(MMU::align_directory(Memory_Map::RAM_BASE)), j = 0; i < MMU::directory(MMU::align_directory(Memory_Map::RAM_BASE)) + n_pts; i++, j++)
        sys_pd[i] = MMU::phy2pde((si->pmm.phy_mem_pts + j * sizeof(Page)));

    //assert((MMU::directory(MMU::align_directory(PHY_MEM)) + n_pts) < (MMU::PD_ENTRIES - 4)); // check if it would overwrite the OS
    for(unsigned int i = MMU::directory(MMU::align_directory(Memory_Map::PHY_MEM)), j = 0; i < MMU::directory(MMU::align_directory(Memory_Map::PHY_MEM)) + n_pts; i++, j++)
        sys_pd[i] = MMU::phy2pde((si->pmm.phy_mem_pts + j * sizeof(Page)));

    unsigned int io_size = MMU::pages(si->bm.mio_top - si->bm.mio_base);
    n_pts = MMU::page_tables(io_size);
    pts = reinterpret_cast<PT_Entry *>(si->pmm.io_pts);
    for(unsigned int i = 0; i < io_size; i++)
        pts[i] = MMU::phy2pte((si->bm.mio_base + i * sizeof(Page)), Flags::MIO);

    //assert((MMU::directory(MMU::align_directory(IO)) + n_pts) < (MMU::PD_ENTRIES - 3)); // check if it would overwrite the OS
    for(unsigned int i = MMU::directory(MMU::align_directory(Memory_Map::IO)), j = 0; i < MMU::directory(MMU::align_directory(Memory_Map::IO)) + n_pts; i++, j++)
        sys_pd[i] = MMU::phy2pde((si->pmm.io_pts + j * sizeof(Page)));

    sys_pd[MMU::directory(Memory_Map::SYS)] = MMU::phy2pde(si->pmm.sys_pt);

    db<Setup>(INF) << "SYS_PD=" << *reinterpret_cast<Page_Table *>(sys_pd) << endl;

}

void Setup::call_next()
{
    db<Setup>(INF) << "SETUP ends here!" << endl;

    // Call the next stage
    static_cast<void (*)()>(_start)();
    //CPU::sepc_write(CPU::Reg(&_start));

    // SETUP is now part of the free memory and this point should never be reached, but, just in case ... :-)
    db<Setup>(ERR) << "OS failed to init!" << endl;
}

__END_SYS

using namespace EPOS::S;

void _entry() // machine mode
{
    // Desabilita as interrupções
    CPU::mstatusc(CPU::MIE);
    // Escreve o mode e o MIE antigo
    CPU::mstatus(CPU::MPP_S | CPU::MPIE);
    // set the stack pointer, thus creating a stack for SETUP
    CPU::sp(Memory_Map::BOOT_STACK + Traits<Machine>::STACK_SIZE - sizeof(long));

    // Salva o hartid para poder usar em s-mode
    Reg core = CPU::mhartid();
    CPU::tp(core);

    // Guarantee that paging is off before going to S-mode.
    CPU::satp(0);

    // Delegar as instrucoes e as excecoes
    CPU::mideleg(CPU::SSI | CPU::STI | CPU::SEI);
    CPU::medeleg(0xffff);

    // Escreve as interrupcoes que vao para s-mode e o handler
    CPU::mies(CPU::MSI | CPU::MTI | CPU::MEI);
    CLINT::mtvec(CLINT::DIRECT, CPU::Reg(&_mmode_forward));

    // Coloca o _setup no pc e entra no modo supervisor
    CPU::mepc((CPU::Reg)&_setup);
    CPU::mret();
}

void _setup() // supervisor mode
{
    CPU::sie(CPU::SSI | CPU::STI | CPU::SEI);
    CPU::sstatus(CPU::SPP_S);
    CLINT::stvec(CLINT::DIRECT, CPU::Reg(&_int_entry));

    Setup setup;
}
