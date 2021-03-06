
#include "kernel.h"
#include "pci.h"
#include "driver_interface.h"

struct Spinlock {
    s32 value = 0;
};

void spinlock_wait(Spinlock *lock) {
    lock->value--;
    while (lock->value != 0) asm("hlt");
}

void spinlock_release(Spinlock *lock) {
	// hmm... we cant acutally wait here because if
	// we call this from an interrupt before spinlock_wait is called
	// then we loop forever
    // while (lock->value == 0) asm("hlt");
    
    lock->value++;
}

#define PCI_IDE_COMPAT_PRIMARY_COMMAND_BLOCK_START 0x01F0
#define PCI_IDE_COMPAT_PRIMARY_CONTROL_BLOCK_START 0x03F4
#define PCI_IDE_COMPAT_PIMARY_IRQ 14

#define PCI_IDE_COMPAT_SECONDARY_COMMAND_BLOCK_START 0x0170
#define PCI_IDE_COMPAT_SECONDARY_CONTROL_BLOCK_START 0x0374
#define PCI_IDE_COMPAT_SECONDARY_IRQ 15

#define PCI_IDE_PROG_IF_PRIMARY_MODE_BIT    (1 << 0)
#define PCI_IDE_PROG_IF_PRIMARY_FIXED_BIT   (1 << 1)
#define PCI_IDE_PROG_IF_SECONDARY_MODE_BIT  (1 << 2)
#define PCI_IDE_PROG_IF_SECONDARY_FIXED_BIT (1 << 3)

#define PCI_IDE_DATA_REGISTER           0
#define PCI_IDE_ERROR_READ_REGISTER     1
#define PCI_IDE_FEATURES_WRITE_REGISTER 1
#define PCI_IDE_SECTOR_COUNT_REGISTER   2
#define PCI_IDE_LBALO_REGISTER          3
#define PCI_IDE_LBAMID_REGISTER         4
#define PCI_IDE_LBAHI_REGISTER          5
#define PCI_IDE_DRIVE_HEAD_REGISTER     6
#define PCI_IDE_STATUS_READ_REGISTER    7
#define PCI_IDE_COMMAND_WRITE_REGISTER  7

#define PCI_IDE_ALT_STATUS_READ_REGISTER      0x2
#define PCI_IDE_DEVICE_CONTROL_WRITE_REGISTER 0x2
#define PCI_IDE_DRIVE_ADDRESS_READ_REGISTER   1

#define PCI_IDE_STATUS_ERR_BIT  (1 << 0)
#define PCI_IDE_STATUS_IDX_BIT  (1 << 1)
#define PCI_IDE_STATUS_CORR_BIT (1 << 2)
#define PCI_IDE_STATUS_DRQ_BIT  (1 << 3)
#define PCI_IDE_STATUS_SRV_BIT  (1 << 4)
#define PCI_IDE_STATUS_DF_BIT   (1 << 5)
#define PCI_IDE_STATUS_RDY_BIT  (1 << 6)
#define PCI_IDE_STATUS_BSY_BIT  (1 << 7)

#define PCI_IDE_COMMAND_IDENTIFY 0xEC
#define PCI_IDE_COMMAND_READ_SECTORS 0x20
#define PCI_IDE_COMMAND_WRITE_SECTORS 0x30

#define PCI_IDE_DRIVE_MASTER 0
#define PCI_IDE_DRIVE_SLAVE  1

#define PCI_IDE_DRIVE_TYPE_ATA   0
#define PCI_IDE_DRIVE_TYPE_ATAPI 1

struct IDE_Driver {
    u16 command_block;
    u16 control_block;
    u8 selected_drive = 0xFF;
    bool is_compat_mode;
    
    struct {
        u8 type;
    } drive_info[2];
    
    Spinlock irq_wait_lock;
    
    void flush_cache() {
        _port_io_write_u8(command_block + PCI_IDE_COMMAND_WRITE_REGISTER, 0xE7);
    }
    
    void send_cmd_reset() {
        // @Cleanup document these bits using #defines
        io_write_u8(control_block + PCI_IDE_DEVICE_CONTROL_WRITE_REGISTER, (1 << 2));
        //udelay(5);
        
        io_write_u8(control_block + PCI_IDE_DEVICE_CONTROL_WRITE_REGISTER, (1 << 1));
        //mdelay(5);
    }
    
    u8 read_ctrl_u8(s8 reg) {
        return _port_io_read_u8(control_block + reg);
    }
    
    void write_ctrl_u8(s8 reg, u8 value) {
        _port_io_write_u8(control_block + reg, value);
    }
    
    u16 read_cmd_u16(s8 reg) {
        return _port_io_read_u16(command_block + reg);
    }
    
    u8 read_cmd_u8(s8 reg) {
        return _port_io_read_u8(command_block + reg);
    }
    
    void write_cmd_u8(s8 reg, u8 value) {
        _port_io_write_u8(command_block + reg, value);
    }
    
    void write_cmd_u16(s8 reg, u16 value) {
        _port_io_write_u16(command_block + reg, value);
    }
    
    u8 wait_for_flags_clear(u8 flags) {
        u8 status = read_cmd_u8(PCI_IDE_STATUS_READ_REGISTER);
        while (status & flags) status = read_cmd_u8(PCI_IDE_STATUS_READ_REGISTER);
        return status;
    }
    
    u8 wait_for_any_flags_set(u8 flags) {
        u8 status = read_cmd_u8(PCI_IDE_STATUS_READ_REGISTER);
        while ( !(status & flags) ) status = read_cmd_u8(PCI_IDE_STATUS_READ_REGISTER);
        return status;
    }
    
    u8 select_drive(u8 drive) {
        kassert( (drive == PCI_IDE_DRIVE_MASTER) || (drive == PCI_IDE_DRIVE_SLAVE) );
        
        if (selected_drive != drive) {
            if (drive == PCI_IDE_DRIVE_MASTER) _port_io_write_u8(command_block + PCI_IDE_DRIVE_HEAD_REGISTER, 0xA0);
            else if (drive == PCI_IDE_DRIVE_SLAVE) _port_io_write_u8(command_block + PCI_IDE_DRIVE_HEAD_REGISTER, 0xB0);
            
            selected_drive = drive;
            
            _io_wait();
            u8 status = read_ctrl_u8(PCI_IDE_ALT_STATUS_READ_REGISTER);
            status = read_ctrl_u8(PCI_IDE_ALT_STATUS_READ_REGISTER);
            status = read_ctrl_u8(PCI_IDE_ALT_STATUS_READ_REGISTER);
            status = read_ctrl_u8(PCI_IDE_ALT_STATUS_READ_REGISTER);
            return status;
        } else {
            return read_ctrl_u8(PCI_IDE_ALT_STATUS_READ_REGISTER);
        }
    }
    
    
    s64 read_sectors_lba28(void *data, u8 sector_count, u32 lba) {
        u8 high4 = (lba >> 24) & 0xF;
        if (selected_drive == PCI_IDE_DRIVE_MASTER)     write_cmd_u8(PCI_IDE_DRIVE_HEAD_REGISTER, 0xE0 | high4);
        else if (selected_drive == PCI_IDE_DRIVE_SLAVE) write_cmd_u8(PCI_IDE_DRIVE_HEAD_REGISTER, 0xF0 | high4);
        
        u8 hi = (lba >> 16) & 0xFF;
        u8 mid = (lba >> 8) & 0xFF;
        u8 lo = (lba >> 0) & 0xFF;
        
        write_cmd_u8(PCI_IDE_SECTOR_COUNT_REGISTER, sector_count);
        write_cmd_u8(PCI_IDE_LBALO_REGISTER, lo);
        write_cmd_u8(PCI_IDE_LBAMID_REGISTER, mid);
        write_cmd_u8(PCI_IDE_LBAHI_REGISTER, hi);
        
        write_cmd_u8(PCI_IDE_COMMAND_WRITE_REGISTER, PCI_IDE_COMMAND_READ_SECTORS);
        
        wait_for_flags_clear(PCI_IDE_STATUS_BSY_BIT);
        u8 status = wait_for_any_flags_set(PCI_IDE_STATUS_DRQ_BIT | PCI_IDE_STATUS_ERR_BIT);
        if (status & PCI_IDE_STATUS_ERR_BIT) return -1;
        
        return _raw_read(data, sector_count * 256 * sizeof(u16), nullptr);
    }
    
    s64 write_sectors_lba28(void *data, u8 sector_count, u32 lba) {
        u8 high4 = (lba >> 24) & 0xF;
        if (selected_drive == PCI_IDE_DRIVE_MASTER)     write_cmd_u8(PCI_IDE_DRIVE_HEAD_REGISTER, 0xE0 | high4);
        else if (selected_drive == PCI_IDE_DRIVE_SLAVE) write_cmd_u8(PCI_IDE_DRIVE_HEAD_REGISTER, 0xF0 | high4);
        
        u8 hi = (lba >> 16) & 0xFF;
        u8 mid = (lba >> 8) & 0xFF;
        u8 lo = (lba >> 0) & 0xFF;
        
        write_cmd_u8(PCI_IDE_SECTOR_COUNT_REGISTER, sector_count);
        write_cmd_u8(PCI_IDE_LBALO_REGISTER, lo);
        write_cmd_u8(PCI_IDE_LBAMID_REGISTER, mid);
        write_cmd_u8(PCI_IDE_LBAHI_REGISTER, hi);
        
        write_cmd_u8(PCI_IDE_COMMAND_WRITE_REGISTER, PCI_IDE_COMMAND_WRITE_SECTORS);
        
        wait_for_flags_clear(PCI_IDE_STATUS_BSY_BIT);
        u8 status = wait_for_any_flags_set(PCI_IDE_STATUS_DRQ_BIT | PCI_IDE_STATUS_ERR_BIT);
        if (status & PCI_IDE_STATUS_ERR_BIT) return -1;
        
        return _raw_write(data, sector_count * 256 * sizeof(u16), nullptr);
    }
    
    // internal use
    s64 _raw_read(void *data, u64 count, u64 *bytes_read) {
        kassert( (count & 1) == 0);
        u16 *data16 = reinterpret_cast<u16 *>(data);
        
        for (u64 i = 0; i < count; i += 2) {
            u16 val = read_cmd_u16(PCI_IDE_DATA_REGISTER);
            *data16 = val;
            data16++;
        }
        
        if (bytes_read) *bytes_read = count;
        
        // @TODO errors
        return 0;
    }
    
    s64 _raw_write(void *data, u64 count, u64 *bytes_written) {
        kassert( (count & 1) == 0);
        u16 *data16 = reinterpret_cast<u16 *>(data);
        
        for (u64 i = 0; i < count; i += 2) {
            write_cmd_u16(PCI_IDE_DATA_REGISTER, *data16);
            data16++;
        }
        
        flush_cache();
        
        if (bytes_written) *bytes_written = count;
        
        // @TODO errors
        return 0;
    }
    
} ide_drivers[2];

irq_result_type ide_irq_handler(s32 irq, void *dev) {
    IDE_Driver *ide = reinterpret_cast<IDE_Driver *>(dev);
    UNUSED(ide);
    // spinlock_release(ide->irq_wait_lock);
    
    // @TODO we have to read the regular status register here in order to tell the drive we intercepted the IRQ
    // @TODO determine that the IRQ actually came from the IDE drive
    return IRQ_RESULT_HANDLED;
}

u8 ide_get_status_400ns(IDE_Driver *ide) {
    _io_wait();
    u8 status = ide->read_ctrl_u8(PCI_IDE_ALT_STATUS_READ_REGISTER);
    status = ide->read_ctrl_u8(PCI_IDE_ALT_STATUS_READ_REGISTER);
    status = ide->read_ctrl_u8(PCI_IDE_ALT_STATUS_READ_REGISTER);
    status = ide->read_ctrl_u8(PCI_IDE_ALT_STATUS_READ_REGISTER);
    return status;
}


s8 ide_send_cmd_identify(IDE_Driver *ide, u16 *buffer) {
    ide->write_cmd_u8(PCI_IDE_SECTOR_COUNT_REGISTER, 1);
    ide->write_cmd_u8(PCI_IDE_LBALO_REGISTER, 0);
    ide->write_cmd_u8(PCI_IDE_LBAMID_REGISTER, 0);
    ide->write_cmd_u8(PCI_IDE_LBAHI_REGISTER, 0);
    ide->write_cmd_u8(PCI_IDE_COMMAND_WRITE_REGISTER, PCI_IDE_COMMAND_IDENTIFY);
    
    // spinlock_wait(ide_primary_driver.irq_wait_lock);
    
    u8 status = ide->read_cmd_u8(PCI_IDE_STATUS_READ_REGISTER);
    if (status == 0) {
        kprint("IDE drive isnt attached.\n");
        return -1;
    }
    
    status = ide->wait_for_flags_clear(PCI_IDE_STATUS_BSY_BIT);
    
    u8 sector_count = ide->read_cmd_u8(PCI_IDE_SECTOR_COUNT_REGISTER);
    u8 lbalo  = ide->read_cmd_u8(PCI_IDE_LBALO_REGISTER);
    u8 lbamid = ide->read_cmd_u8(PCI_IDE_LBAMID_REGISTER);
    u8 lbahi  = ide->read_cmd_u8(PCI_IDE_LBAHI_REGISTER);
    
    
    if (sector_count == 0x1 && lbalo == 0x1 && lbamid == 0x14 && lbahi == 0xEB) {
        ide->drive_info[ide->selected_drive].type = PCI_IDE_DRIVE_TYPE_ATAPI;
        
        // @TODO
        // } else if (sector_count == 0x1 && lbalo == 0x1 && lbamid == 0 && lbahi == 0)  {
    } else if (true) {
        ide->drive_info[ide->selected_drive].type = PCI_IDE_DRIVE_TYPE_ATA;
    } else {
        kprint("Drive signature not recognized: %d, %d, %X, %X\n", sector_count, lbalo, lbamid, lbahi);
        return -1;
    }
    
    status = ide->wait_for_any_flags_set(PCI_IDE_STATUS_DRQ_BIT | PCI_IDE_STATUS_ERR_BIT);
    
    if (status & PCI_IDE_STATUS_ERR_BIT) {
        // @TODO error handle, note that SATA and ATAPI drives always set the error bit but send their PIO data anyways
        kprint("Drive returned error\n");
        return -1;
    }
    
    status = ide_get_status_400ns(ide);
    
    ide->read_sectors_lba28(buffer, 1, 0);
    return 0;
}

void setup_ide_driver(Pci_Device_Config *header, IDE_Driver *ide, u16 command_block, u16 control_block) {
    u8 prog_if = header->prog_if;
    
    ide->is_compat_mode = ((prog_if & PCI_IDE_PROG_IF_PRIMARY_MODE_BIT) == 0);
    if (ide->is_compat_mode) {
        ide->command_block = command_block;
        ide->control_block = control_block;
    }
    ide->selected_drive = 0xFF;
    
    u8 status = ide->read_cmd_u8(PCI_IDE_STATUS_READ_REGISTER);
    if (status == 0xFF){
        // @TODO ErrorCode
        
        kprint("IDE Controller at IO(%X) isnt attached\n", command_block);
        return;
    }
    
    kassert(ide->selected_drive == 0xFF);
    
    // install irq handler
    // @FixMe do this for both controller drivers
    // if we have two controllers
    // register_irq_handler(14, "IDE Controller", ide_irq_handler, ide_primary_driver);
    
    ide->write_cmd_u8(PCI_IDE_DRIVE_HEAD_REGISTER, 0);
    ide->write_ctrl_u8(PCI_IDE_DEVICE_CONTROL_WRITE_REGISTER, 1 << 2);
    ide->write_ctrl_u8(PCI_IDE_DEVICE_CONTROL_WRITE_REGISTER, 0);
    
    // void clear_irq_mask(u8 irq_line);
    // clear_irq_mask(2);
    // clear_irq_mask(14);
    
    u16 buffer[1024];
    zero_memory(&buffer, 512);
    
    ide->select_drive(PCI_IDE_DRIVE_MASTER);
    if (ide_send_cmd_identify(ide, &buffer[0]) == 0) {
        kprint("IDE BUF[ 12]: %X\n", buffer[12]);
        kprint("IDE BUF[ 90]: %X\n", buffer[90]);
        kprint("IDE BUF[100]: %X\n", buffer[100]);
        kprint("IDE BUF[200]: %X\n", buffer[200]);
        
        ide->read_sectors_lba28(&buffer, 1, 0);
        
        kprint("Post-IDE read\n");
        kprint("IDE BUF[ 12]: %X\n", buffer[12]);
        kprint("IDE BUF[ 90]: %X\n", buffer[90]);
        kprint("IDE BUF[100]: %X\n", buffer[100]);
        kprint("IDE BUF[200]: %X\n", buffer[200]);
    }
    
    ide->select_drive(PCI_IDE_DRIVE_SLAVE);
    if (ide_send_cmd_identify(ide, &buffer[0]) == 0) {
        kprint("IDE BUF[ 12]: %X\n", buffer[12]);
        kprint("IDE BUF[ 90]: %X\n", buffer[90]);
        kprint("IDE BUF[100]: %X\n", buffer[100]);
        kprint("IDE BUF[200]: %X\n", buffer[200]);
        
        ide->read_sectors_lba28(&buffer, 1, 0);
        
        kprint("Post-IDE read\n");
        kprint("IDE BUF[ 12]: %X\n", buffer[12]);
        kprint("IDE BUF[ 90]: %X\n", buffer[90]);
        kprint("IDE BUF[100]: %X\n", buffer[100]);
        kprint("IDE BUF[200]: %X\n", buffer[200]);
    }
    
    // buffer[12] = 0xBEEF;
    // buffer[90] = 0xCAFE;
    // buffer[100] = 0xBEEF;
    // buffer[200] = 0xCAFE;
    
    // ide_device_write_sectors_lba28(ide_primary_driver, &buffer, 1, 0);
    // zero_memory(&buffer, sizeof(buffer));
}

void create_ide_driver(Pci_Device_Config *header) {
    kassert( (header->header_type & (~PCI_HEADER_MULTIFUNCTION_BIT)) == 0);
    
    kprint("BAR0: %X\n", header->type_00.bar0);
    kprint("BAR1: %X\n", header->type_00.bar1);
    kprint("BAR2: %X\n", header->type_00.bar2);
    kprint("BAR3: %X\n", header->type_00.bar3);
    kprint("BAR4: %X\n", header->type_00.bar4);
    kprint("BAR5: %X\n", header->type_00.bar5);
    kprint("ProgIF: %X\n", header->prog_if);
    setup_ide_driver(header, &ide_drivers[0], PCI_IDE_COMPAT_PRIMARY_COMMAND_BLOCK_START, PCI_IDE_COMPAT_PRIMARY_CONTROL_BLOCK_START);
    setup_ide_driver(header, &ide_drivers[1], PCI_IDE_COMPAT_SECONDARY_COMMAND_BLOCK_START, PCI_IDE_COMPAT_SECONDARY_CONTROL_BLOCK_START);
}
