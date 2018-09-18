
#include "kernel.h"
#include "pci.h"
#include "driver_interface.h"

struct Spinlock {
    s32 value = 0;
};

void spinlock_attain(Spinlock *lock) {
    lock->value--;
    while (lock->value != 0) asm("hlt");
}

void spinlock_release(Spinlock *lock) {
    while (lock->value == 0) asm("hlt");

    lock->value++;
}

#define PCI_IDE_COMPAT_PRIMARY_COMMAND_BLOCK_START 0x01F0
#define PCI_IDE_COMPAT_PRIMARY_CONTROL_BLOCK_START 0x03F6
#define PCI_IDE_COMPAT_PIMARY_IRQ 14

#define PCI_IDE_COMPAT_SECONDARY_COMMAND_BLOCK_START 0x0170
#define PCI_IDE_COMPAT_SECONDARY_CONTROL_BLOCK_START 0x0376
#define PCI_IDE_COMPAT_SECONDARY_IRQ 15

#define PCI_IDE_PROG_IF_PRIMARY_MODE_BIT    (1 << 0)
#define PCI_IDE_PROG_IF_PRIMARY_FIXED_BIT   (1 << 1)
#define PCI_IDE_PROG_IF_SECONDARY_MODE_BIT  (1 << 2)
#define PCI_IDE_PROG_IF_SECONDARY_FIXED_BIT (1 << 3)

#define PCI_IDE_DATA_REGISTER           0
#define PCI_IDE_ERROR_READ_REGISTER     1
#define PCI_IDE_FEATURES_WRITE_REGISTER 1
#define PCI_IDE_SECTOR_COUNT_REGISTER   2
#define PCI_IDE_SECTOR_NUMBER_REGISTER  3
#define PCI_IDE_CYLINDER_LOW_REGISTER   4
#define PCI_IDE_CYLINDER_HIGH_REGISTER  5
#define PCI_IDE_DRIVE_HEAD_REGISTER     6
#define PCI_IDE_STATUS_READ_REGISTER    7
#define PCI_IDE_COMMAND_WRITE_REGISTER  7

#define PCI_IDE_ALT_STATUS_READ_REGISTER      0
#define PCI_IDE_DEVICE_CONTROL_WRITE_REGISTER 0
#define PCI_IDE_DRIVE_ADDRESS_READ_REGISTER   1

#define PCI_IDE_DRIVE_MASTER 0
#define PCI_IDE_DRIVE_SLAVE  1

#define PCI_IDE_COMMAND_IDENTIFY 0xEC

struct IDE_Driver {
    u16 command_block;
    u16 control_block;
    u8 selected_drive = 0xFF;
    bool is_compat_mode;

    Spinlock irq_wait_lock;
} ide_primary_driver;

irq_result_type ide_irq_handler(s32 irq, void *dev) {
    IDE_Driver *ide = reinterpret_cast<IDE_Driver *>(dev);
    spinlock_release(&ide->irq_wait_lock);

    // @TODO we have to read the regular status register here in order to tell the drive we intercepted the IRQ
    // @TODO determine that the IRQ actually came from the IDE drive
    return IRQ_RESULT_HANDLED;
}

void ide_flush_device_cache(IDE_Driver *ide) {
	_port_io_write_u8(ide->command_block + PCI_IDE_COMMAND_WRITE_REGISTER, 0xE7);
}

u8 ide_read_cmd_reg_u8(IDE_Driver *ide, s8 reg) {
	return _port_io_read_u8(ide->command_block + reg);
}

void ide_write_cmd_reg_u8(IDE_Driver *ide, s8 reg, u8 value) {
	_port_io_write_u8(ide->command_block + reg, value);
}

u8 ide_read_ctrl_reg_u8(IDE_Driver *ide, s8 reg) {
	return _port_io_read_u8(ide->control_block + reg);
}

void ide_write_ctrl_reg_u8(IDE_Driver *ide, s8 reg, u8 value) {
	_port_io_write_u8(ide->control_block + reg, value);
}

u8 ide_select_drive(IDE_Driver *ide, u8 drive) {
	kassert( (drive == PCI_IDE_DRIVE_MASTER) || (drive == PCI_IDE_DRIVE_SLAVE) );

	if (ide->selected_drive != drive) {
		if (drive == PCI_IDE_DRIVE_MASTER) _port_io_write_u8(ide->command_block + PCI_IDE_DRIVE_HEAD_REGISTER, 0xA0);
		else if (drive == PCI_IDE_DRIVE_SLAVE) _port_io_write_u8(ide->command_block + PCI_IDE_DRIVE_HEAD_REGISTER, 0xB0);

		ide->selected_drive = drive;
		
		_io_wait();
		u8 status = ide_read_ctrl_reg_u8(ide, PCI_IDE_ALT_STATUS_READ_REGISTER);
		status = ide_read_ctrl_reg_u8(ide, PCI_IDE_ALT_STATUS_READ_REGISTER);
		status = ide_read_ctrl_reg_u8(ide, PCI_IDE_ALT_STATUS_READ_REGISTER);
		status = ide_read_ctrl_reg_u8(ide, PCI_IDE_ALT_STATUS_READ_REGISTER);
		return status;
	} else {
		return ide_read_ctrl_reg_u8(ide, PCI_IDE_ALT_STATUS_READ_REGISTER);
	}
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
    u8 prog_if = header->prog_if;

    ide_primary_driver.is_compat_mode = ((prog_if & PCI_IDE_PROG_IF_PRIMARY_MODE_BIT) == 0);
    if (ide_primary_driver.is_compat_mode) {
        ide_primary_driver.command_block = PCI_IDE_COMPAT_PRIMARY_COMMAND_BLOCK_START;
        ide_primary_driver.control_block = PCI_IDE_COMPAT_PRIMARY_CONTROL_BLOCK_START;
    }
    ide_primary_driver.selected_drive = 0xFF;

    u8 status = ide_read_cmd_reg_u8(&ide_primary_driver, PCI_IDE_STATUS_READ_REGISTER);
    if (status == 0xFF){
    	// @TODO ErrorCode

    	kprint("Primary IDE Controller isnt attached\n");
    	return;
    }

    kassert(ide_primary_driver.selected_drive == 0xFF);

    // install irq handler
    register_irq_handler(14, "IDE Controller", ide_irq_handler, &ide_primary_driver);

    ide_write_cmd_reg_u8(&ide_primary_driver, PCI_IDE_DRIVE_HEAD_REGISTER, 0);
    ide_write_ctrl_reg_u8(&ide_primary_driver, PCI_IDE_DEVICE_CONTROL_WRITE_REGISTER, 1 << 2);
    ide_write_ctrl_reg_u8(&ide_primary_driver, PCI_IDE_DEVICE_CONTROL_WRITE_REGISTER, 0);

    void clear_irq_mask(u8 irq_line);
    clear_irq_mask(2);
    clear_irq_mask(14);

    ide_select_drive(&ide_primary_driver, PCI_IDE_DRIVE_MASTER);
    ide_write_cmd_reg_u8(&ide_primary_driver, PCI_IDE_SECTOR_COUNT_REGISTER, 1);
    ide_write_cmd_reg_u8(&ide_primary_driver, PCI_IDE_SECTOR_NUMBER_REGISTER, 0);
    ide_write_cmd_reg_u8(&ide_primary_driver, PCI_IDE_CYLINDER_LOW_REGISTER, 0);
    ide_write_cmd_reg_u8(&ide_primary_driver, PCI_IDE_CYLINDER_HIGH_REGISTER, 0);
    ide_write_cmd_reg_u8(&ide_primary_driver, PCI_IDE_COMMAND_WRITE_REGISTER, PCI_IDE_COMMAND_IDENTIFY);

    spinlock_attain(&ide_primary_driver.irq_wait_lock);

    status = ide_read_cmd_reg_u8(&ide_primary_driver, PCI_IDE_STATUS_READ_REGISTER);
    if (status == 0) {
    	kprint("IDE Master Drive isnt attached.\n");
    	return;
    } else {
    	kprint("IDE Status: %X\n", status);
    }

}
