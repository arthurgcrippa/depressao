#include <architecture/mmu.h>
#include <system.h>

__BEGIN_SYS

void MMU::init()
{
    typedef MMU::Phy_Addr Phy_Addr;

    db<Init, MMU>(INF) << "MMU::init()" << endl;
    System_Info * si = System::info();
    unsigned sys_data_end = si->lm.sys_data + si->lm.sys_data_size + 1;

    db<Init, MMU>(INF) << "Sys Data End: " << (Phy_Addr) sys_data_end << endl;

    db<Init, MMU>(INF) << "Memory Map Sys Data End: " << (Phy_Addr) (Memory_Map::SYS_DATA + si->lm.sys_data_size + 1) << endl;



    free(align_page(Memory_Map::SYS_DATA + si->lm.sys_data_size + 1), pages(Memory_Map::SYS_HIGH - align_page(Memory_Map::SYS_DATA + si->lm.sys_data_size + 1))); // [align_page(&_end), 0x87bf9000]
    free(Memory_Map::RAM_TOP + 1 - Traits<Machine>::STACK_SIZE * Traits<Machine>::CPUS, pages(Traits<Machine>::STACK_SIZE * Traits<Machine>::CPUS));
    // Free init/setup memory
    free(Memory_Map::RAM_BASE, pages(Memory_Map::MMODE_F - Memory_Map::RAM_BASE));

}

__END_SYS
