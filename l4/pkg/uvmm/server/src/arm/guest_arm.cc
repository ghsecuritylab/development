/*
 * Copyright (C) 2015 Kernkonzept GmbH.
 * Author(s): Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *
 * This file is distributed under the terms of the GNU General Public
 * License, version 2.  Please see the COPYING-GPL-2 file for details.
 */

#include <l4/cxx/unique_ptr>
#include <l4/cxx/ref_ptr>
#include <l4/re/error_helper>
#include <l4/vbus/vbus>

#include "binary_loader.h"
#include "device_factory.h"
#include "guest.h"
#include "guest_subarch.h"
#include "irq.h"
#include "irq_dt.h"
#include "pm.h"
#include "virt_bus.h"

static cxx::unique_ptr<Vmm::Guest> guest;

__thread unsigned vmm_current_cpu_id;

typedef void (*Entry)(Vmm::Vcpu_ptr vcpu);

namespace Vmm {

Guest::Guest()
: _gic(Vdev::make_device<Gic::Dist>(16, Vmm::Cpu_dev::Max_cpus))
{}

Guest *
Guest::create_instance()
{
  guest.reset(new Guest());
  return guest.get();
}

namespace {

using namespace Vdev;

struct F : Factory
{
  cxx::Ref_ptr<Vdev::Device> create(Device_lookup *devs,
                                    Vdev::Dt_node const &node) override
  {
    auto *vbus = devs->vbus().get();
    if (!vbus->io_ds())
      {
        Err().printf("ERROR: ARM GIC virtualization does not work without passing the virtual GICC via the vbus\n");
        return nullptr; // missing hardware part, disable GIC
      }

    // attach GICD to VM
    auto gic = devs->vmm()->gic();
    devs->vmm()->register_mmio_device(gic, node);

    Virt_bus::Devinfo *devinfo =
      vbus->find_unassigned_device_by_hid("arm-gicc");
    if (!devinfo)
      L4Re::chksys(-L4_ENODEV, "getting ARM GIC from IO");

    l4vbus_resource_t res;
    L4Re::chksys(devinfo->io_dev().get_resource(0, &res),
                 "getting memory resource");

    Dbg(Dbg::Irq, Dbg::Info, "GIC").printf("ARM GIC: %08lx-%08lx\n",
                                           res.start, res.end);

    auto g2 = Vdev::make_device<Ds_handler>(vbus->io_ds(), 0,
                                            res.end - res.start + 1, res.start);
    devs->vmm()->register_mmio_device(g2, node, 1);
    devinfo->set_proxy(gic);
    return gic;
  }
};

static F f;
static Vdev::Device_type t1 = { "arm,cortex-a9-gic", nullptr, &f };
static Vdev::Device_type t2 = { "arm,cortex-a15-gic", nullptr, &f };
static Vdev::Device_type t3 = { "arm,cortex-a7-gic", nullptr, &f };
static Vdev::Device_type t4 = { "arm,gic-400", nullptr, &f };

struct F_timer : Factory
{
  cxx::Ref_ptr<Vdev::Device> create(Device_lookup *devs,
                                    Vdev::Dt_node const &node) override
  {
    Vdev::Irq_dt_iterator it(devs, node);

    // skip the first two interrupts
    for (int i = 0; i < 3; ++i)
      L4Re::chksys(it.next(devs), "Parsing timer interrupt");

    if (!it.ic_is_virt())
      L4Re::chksys(-L4_EINVAL, "Timer not connected to virtual interrupt controller");

    auto timer = Vdev::make_device<Vdev::Core_timer>(it.ic().get(), it.irq(), node);

    devs->vmm()->set_timer(timer);
    return timer;
  }
};

static F_timer ftimer;
static Vdev::Device_type tt1 = { "arm,armv7-timer", nullptr, &ftimer };
static Vdev::Device_type tt2 = { "arm,armv8-timer", nullptr, &ftimer };

} // namespace

static Dt_node
get_psci_node(Device_tree const &dt)
{
  char const *compats[] = { "arm,psci", "arm,psci-0.2", "arm,psci-1.0" };

  for (auto compat : compats)
    {
      auto node = dt.first_compatible_node(compat);
      if (node.is_valid())
        return node;
    }
  return Dt_node();
}

void
Guest::setup_device_tree(Vdev::Device_tree dt)
{
  Dt_node parent;
  Dt_node node = get_psci_node(dt);

  if (node.is_valid())
    {
      parent = node.parent_node();
      if (node.del_node() < 0)
        {
          Err().printf("Failed to delete %s, unable to set psci methods\n",
                       node.get_name());
          return;
        }
    }
  else
    parent = dt.first_node();

  node = parent.add_subnode("psci");
  if (!node.is_valid())
    return;

  node.setprop_string("compatible", "arm,psci-0.2");
  node.setprop_string("method", "hvc");
}

void
Guest::check_guest_constraints(l4_addr_t base) const
{
  Dbg warn(Dbg::Mmio, Dbg::Warn, "ram");

  if (guest_64bit)
    {
      if (base & ((1UL << 21) - 1))
        warn.printf(
          "\033[01;31mWARNING: Guest memory not 2MB aligned!\033[m\n"
          "       If you run a 64bit-Linux as a guest,\n"
          "       Linux will likely fail to boot as it expects\n"
          "       a 2MB alignment of its memory.\n"
          "       Current guest RAM alignment is only 0x%x\n",
          1 << __builtin_ctz(base));

      return;
    }

  if (base & ((1UL << 27) - 1))
    warn.printf(
      "\033[01;31mWARNING: Guest memory not 128MB aligned!\033[m\n"
      "       If you run a 32bit-Linux as a guest,\n"
      "       Linux will likely fail to boot as it assumes\n"
      "       a 128MB alignment of its memory.\n"
      "       Current guest RAM alignment is only 0x%x\n",
      1 << __builtin_ctz(base));

  if (base & ~0xf0000000)
    warn.printf(
      "WARNING: Guest memory not 256MB aligned!\n"
      "       If you run a 32bit-Linux as a guest, you might hit a bug\n"
      "       in the arch/arm/boot/compressed/head.S code\n"
      "       that misses an ISB after code has been relocated.\n"
      "       According to the internet a fix for this issue\n"
      "       is floating around.\n");
}

l4_addr_t
Guest::load_linux_kernel(Vm_ram *ram, char const *kernel, Ram_free_list *free_list)
{
  Guest_addr ram_base = free_list->first_free_address();

  l4_addr_t entry = ~0ul;
  Boot::Binary_ds image(kernel);
  if (image.is_elf_binary())
    {
      entry = image.load_as_elf(ram, free_list);
      guest_64bit = image.is_elf64();
      if (!Guest_64bit_supported && guest_64bit)
        L4Re::chksys(-L4_EINVAL, "Running a 64bit guest on a 32bit host is "
                                 "not possible.");
    }
  else
    {
      char const *h = reinterpret_cast<char const *>(image.get_header());

      if (Guest_64bit_supported
          && h[0x38] == 'A' && h[0x39] == 'R'
          && h[0x3A] == 'M' && h[0x3B] == '\x64') // Linux header ARM\x64
        {
          l4_uint64_t l = *reinterpret_cast<l4_uint64_t const *>(&h[8]);
          // Bytes 0xc-0xf have the size
          entry = image.load_as_raw(ram, ram_base + l, free_list);
          this->guest_64bit = true;
        }
      else if (   h[0x24] == 0x18 && h[0x25] == 0x28
               && h[0x26] == 0x6f && h[0x27] == 0x01) // Linux magic
        {
          l4_uint32_t l = *reinterpret_cast<l4_uint32_t const *>(&h[0x28]);
          // Bytes 0x2c-0x2f have the zImage size
          entry = image.load_as_raw(ram, ram_base + l, free_list);
        }

      if (entry == ~0ul)
        {
          enum { Default_entry =  0x208000 };
          entry = image.load_as_raw(ram, ram_base + Default_entry, free_list);
        }
    }

  check_guest_constraints(ram_base.get());

  return entry;
}

  /*
   * Prepare a clean vcpu register state before entering the VM
   *
   * Initializes the VCPU register state according to the mode the
   * VCPU is supposed to run in. Registers related to virtualization
   * (control registers, vcpu state registers) are initialized in the
   * context of the thread handling this virtual CPU.
   * We assume that this state is not changed by invoking
   * vcpu_control_ext().
   */
void
Guest::prepare_vcpu_startup(Vcpu_ptr vcpu, l4_addr_t entry) const
{
  if (Guest_64bit_supported && guest_64bit)
    vcpu->r.flags = Cpu_dev::Flags_default_64;
  else
    {
      vcpu->r.flags = Cpu_dev::Flags_default_32;
      if (entry & 1)
        {
          // set thumb mode, remove thumb bit from address
          vcpu->r.flags |= 1 << 5;
          entry &= ~1;
        }
    }

  vcpu->r.sp    = 0;
  vcpu->r.ip    = entry;
}


void
Guest::prepare_linux_run(Vcpu_ptr vcpu, l4_addr_t entry,
                         Vm_ram * /* ram */, char const * /* kernel */,
                         char const * /* cmd_line */, l4_addr_t dt_boot_addr)
{
  prepare_vcpu_startup(vcpu, entry);

  // Set up the VCPU state as expected by Linux entry
  if (Guest_64bit_supported && guest_64bit)
    {
      vcpu->r.r[0]  = dt_boot_addr;
      vcpu->r.r[1]  = 0;
      vcpu->r.r[2]  = 0;
    }
  else
    {
      vcpu->r.r[0]  = 0;
      vcpu->r.r[1]  = ~0UL;
      vcpu->r.r[2]  = dt_boot_addr;
    }
  vcpu->r.r[3]  = 0;
}

void
Guest::run(cxx::Ref_ptr<Cpu_dev_array> cpus)
{
  if (!_timer)
    warn().printf("WARNING: No timer found. Your guest will likely not work properly!\n");

  _cpus = cpus;
  for (auto cpu: *cpus.get())
    {
      if (!cpu)
        continue;

      auto vcpu = cpu->vcpu();

      vcpu->user_task = _task.cap();
      cpu->powerup_cpu();
      info().printf("Powered up cpu%d [%p]\n", vcpu.get_vcpu_id(),
                    cpu.get());

      _gic->set_cpu(vcpu.get_vcpu_id(), *vcpu, cpu->thread_cap());
    }

  cpus->cpu(0)->startup();
}

l4_msgtag_t
Guest::handle_entry(Vcpu_ptr vcpu)
{
  auto *utcb = l4_utcb();

  process_pending_ipc(vcpu, utcb);
  _gic->schedule_irqs(vmm_current_cpu_id);

  L4::Cap<L4::Thread> myself;
  return myself->vcpu_resume_start(utcb);
}

Cpu_dev *
Guest::lookup_cpu(l4_uint32_t hwid) const
{
  for (auto const &cpu : *_cpus.get())
    if (cpu && cpu->matches(hwid))
      return cpu.get();

  return nullptr;
}

bool
Guest::handle_psci_call(Vcpu_ptr vcpu)
{
  enum Psci_error_codes
  {
    SUCCESS            = 0,
    NOT_SUPPORTED      = -1,
    INVALID_PARAMETERS = -2,
    DENIED             = -3,
    ALREADY_ON         = -4,
    ON_PENDING         = -5,
    INTERNAL_FAILURE   = -6,
    NOT_PRESENT        = -7,
    DISABLED           = -8,
    INVALID_ADDRESS    = -9,
  };

  enum Psci_functions
  {
    PSCI_VERSION          = 0,
    CPU_SUSPEND           = 1,
    CPU_OFF               = 2,
    CPU_ON                = 3,
    AFFINITY_INFO         = 4,
    MIGRATE               = 5,
    MIGRATE_INFO_TYPE     = 6,
    MIGRATE_INFO_UP_CPU   = 7,
    SYSTEM_OFF            = 8,
    SYSTEM_RESET          = 9,
    PSCI_FEATURES         = 10,
    CPU_FREEZE            = 11,
    CPU_DEFAULT_SUSPEND   = 12,
    NODE_HW_STATE         = 13,
    SYSTEM_SUSPEND        = 14,
    PSCI_SET_SUSPEND_MODE = 15,
    PSCI_STAT_RESIDENCY   = 16,
    PSCI_STAT_COUNT       = 17,
  };

  enum Psci_migrate_info
  {
    TOS_UP_MIG_CAP     = 0,
    TOS_NOT_UP_MIG_CAP = 1,
    TOS_NOT_PRESENT_MP = 2,
  };

  if ((vcpu->r.r[0] & 0xbfffff00) != 0x84000000)
    return false;

  bool is64bit = vcpu->r.r[0] & 0x40000000;

  if (is64bit && sizeof(long) == 4)
    {
      vcpu->r.r[0] = NOT_SUPPORTED;
      return true;
    }

  unsigned func = vcpu->r.r[0] & 0xff;
  switch (func)
    {
    case PSCI_VERSION:
      vcpu->r.r[0] = 0x00010000; // v1.0
      break;

    case CPU_SUSPEND:
      {
        l4_addr_t power_state  = vcpu->r.r[1];
        l4_addr_t entry_gpa    = vcpu->r.r[2];
        l4_umword_t context_id = vcpu->r.r[3];

        wait_for_timer_or_irq(vcpu);

        if (power_state & (1 << 30))
          {
            memset(&vcpu->r, 0, sizeof(vcpu->r));
            prepare_vcpu_startup(vcpu, entry_gpa);
            vcpu->r.r[0]  = context_id;
            l4_vcpu_e_write_32(*vcpu, L4_VCPU_E_SCTLR,
                               l4_vcpu_e_read_32(*vcpu, L4_VCPU_E_SCTLR) & ~1U);
          }
        else
          vcpu->r.r[0] = SUCCESS;
      }
      break;

    case CPU_OFF:
      vcpu->r.r[0] = NOT_SUPPORTED;
      Err().printf("... PSCI CPU OFF\n");
      break;

    case CPU_ON:
      {
        unsigned long hwid = vcpu->r.r[1];
        Cpu_dev *target = lookup_cpu(hwid);

        if (target)
          {
            // XXX There is currently no way to detect error conditions like
            // INVALID_ADDRESS or ALREADY_ON
            l4_mword_t ip = vcpu->r.r[2];
            l4_mword_t context =  vcpu->r.r[3];
            target->vcpu()->r.r[0] = context;
            prepare_vcpu_startup(target->vcpu(), ip);
            target->start_vcpu();
            vcpu->r.r[0] = SUCCESS;
          }
        else
          vcpu->r.r[0] = INVALID_PARAMETERS;
      }
      break;

    case MIGRATE_INFO_TYPE:
      vcpu->r.r[0] = TOS_NOT_PRESENT_MP;
      break;

    case SYSTEM_OFF:
      _pm.shutdown();
      exit(0);

    case SYSTEM_RESET:
      _pm.shutdown(true);
      exit(102); // 0x66 is also used by our syscon config

    case PSCI_FEATURES:
        {
          unsigned feat_func = vcpu->r.r[1] & 0xff;
          switch (feat_func)
            {
            case CPU_SUSPEND:
            case SYSTEM_SUSPEND:
              vcpu->r.r[0] = 1 << 1;
              break;
            default:
              vcpu->r.r[0] = NOT_SUPPORTED;
              break;
            };
        }
      break;

    case SYSTEM_SUSPEND:
        {
          l4_addr_t entry_gpa = vcpu->r.r[1];
          l4_umword_t context_id = vcpu->r.r[2];

          // TODO: Check that all other cores are off
          // if not:
          if (0)
            {
              vcpu->r.r[0] = DENIED;
              return true;
            }

          /* Go to sleep */
          if (_pm.suspend())
            wait_for_ipc(l4_utcb(), L4_IPC_NEVER);
          /* Back alive */
          _pm.resume();

          memset(&vcpu->r, 0, sizeof(vcpu->r));
          prepare_vcpu_startup(vcpu, entry_gpa);
          vcpu->r.r[0]  = context_id;
          l4_vcpu_e_write_32(*vcpu, L4_VCPU_E_SCTLR,
                             l4_vcpu_e_read_32(*vcpu, L4_VCPU_E_SCTLR) & ~1U);
        }
      break;

    default:
      Err().printf("... Unknown PSCI function 0x%x called\n", func);
      vcpu->r.r[0] = NOT_SUPPORTED;
      break;
    };

  return true;
}

void
Guest::handle_smc_call(Vcpu_ptr vcpu)
{
  if (_smc_handler)
    _smc_handler->smc(vcpu);
  else
    Err().printf("No handler for SMC call: a0=%lx a1=%lx ip=%lx\n",
                 vcpu->r.r[0], vcpu->r.r[1], vcpu->r.ip);

  vcpu->r.ip += 4;
}


static void dispatch_vm_call(Vcpu_ptr vcpu)
{
  if (guest->handle_psci_call(vcpu))
    return;

  Err().printf("Unknown HVC call: a0=%lx a1=%lx ip=%lx\n",
               vcpu->r.r[0], vcpu->r.r[1], vcpu->r.ip);
}

static void dispatch_smc(Vcpu_ptr vcpu)
{
  guest->handle_smc_call(vcpu);
}

static void
guest_unknown_fault(Vcpu_ptr vcpu)
{
  Err().printf("unknown trap: err=%lx ec=0x%x ip=%lx\n",
               vcpu->r.err, (int)vcpu.hsr().ec(), vcpu->r.ip);
  guest->halt_vm();
}

static void
guest_memory_fault(Vcpu_ptr vcpu)
{
  switch (guest->handle_mmio(vcpu->r.pfa, vcpu))
    {
    case Retry: break;
    case Jump_instr: vcpu.jump_instruction(); break;
    default:
      Err().printf("cannot handle VM memory access @ %lx ip=%lx\n",
                   vcpu->r.pfa, vcpu->r.ip);
      guest->halt_vm();
      break;
    }
}

void
Vmm::Guest::wait_for_timer_or_irq(Vcpu_ptr vcpu)
{
  if (_gic->schedule_irqs(vmm_current_cpu_id))
    return;

  l4_timeout_t to = L4_IPC_NEVER;

  auto *utcb = l4_utcb();
  if (_timer
      && (l4_vcpu_e_read_32(*vcpu, L4_VCPU_E_CNTVCTL) & 3) == 1) // timer enabled and not masked
    {
      // calculate the timeout based on the VTIMER values !
      auto cnt = vcpu.cntvct();
      auto cmp = vcpu.cntv_cval();

      if (cmp <= cnt)
        return;

      l4_uint64_t diff = _timer->get_micro_seconds(cmp - cnt);
      if (0)
        printf("diff=%lld\n", diff);
      l4_rcv_timeout(l4_timeout_abs_u(l4_kip_clock(l4re_kip()) + diff, 8, utcb), &to);
    }

  wait_for_ipc(utcb, to);
}

void
Vmm::Guest::handle_wfx(Vcpu_ptr vcpu)
{
  vcpu->r.ip += 2 << vcpu.hsr().il();
  if (vcpu.hsr().wfe_trapped()) // WFE
    return;

  wait_for_timer_or_irq(vcpu);
}

static void
guest_wfx(Vcpu_ptr vcpu)
{ guest->handle_wfx(vcpu); }


void
Vmm::Guest::handle_ppi(Vcpu_ptr vcpu)
{
  switch (vcpu.hsr().svc_imm())
    {
    case 0: // VGIC IRQ
      _gic->handle_maintenance_irq(vmm_current_cpu_id);
      break;
    case 1: // VTMR IRQ
      if (_timer)
        _timer->inject();
      break;
    default:
      Err().printf("unknown virtual PPI: %d\n", (int)vcpu.hsr().svc_imm());
      break;
    }
}

static void
guest_ppi(Vcpu_ptr vcpu)
{ guest->handle_ppi(vcpu); }

static void guest_irq(Vcpu_ptr vcpu)
{
  guest->handle_ipc(vcpu->i.tag, vcpu->i.label, l4_utcb());
}

static void guest_mcr_access(Vcpu_ptr vcpu)
{
  auto hsr = vcpu.hsr();
  if (   hsr.mcr_opc1() == 0
      && hsr.mcr_crn() == 0
      && hsr.mcr_crm() == 1
      && hsr.mcr_opc2() == 0
      && hsr.mcr_read()) // DCC Status
    {
      // printascii in Linux is doing busyuart which wants to see a
      // busy flag to quit its loop while waituart does not want to
      // see a busy flag; this little trick makes it work
      static l4_umword_t flip;
      flip ^= 1 << 29;
      vcpu.set_gpr(hsr.mcr_rt(), flip);
    }
  else if (   hsr.mcr_opc1() == 0
           && hsr.mcr_crn() == 0
           && hsr.mcr_crm() == 5
           && hsr.mcr_opc2() == 0) // DCC Get/Put
    {
      if (hsr.mcr_read())
        vcpu.set_gpr(hsr.mcr_rt(), 0);
      else
        putchar(vcpu.get_gpr(hsr.mcr_rt()));
    }
  else
    {
      if (   hsr.mcr_opc1() == 0
          && hsr.mcr_crn() == 0
          && hsr.mcr_crm() == 0
          && hsr.mcr_opc2() == 0
          && hsr.mcr_read())
        printf("Unhandled DCC request: Non-ARMv7 guest?\n");

      printf("%08lx: %s p14, %d, r%d, c%d, c%d, %d (hsr=%08lx)\n",
             vcpu->r.ip, hsr.mcr_read() ? "MRC" : "MCR",
             (unsigned)hsr.mcr_opc1(),
             (unsigned)hsr.mcr_rt(),
             (unsigned)hsr.mcr_crn(),
             (unsigned)hsr.mcr_crm(),
             (unsigned)hsr.mcr_opc2(),
             (l4_umword_t)hsr.raw());
    }

  vcpu->r.ip += 2 << hsr.il();
}

extern "C" l4_msgtag_t prepare_guest_entry(Vcpu_ptr vcpu);
l4_msgtag_t prepare_guest_entry(Vcpu_ptr vcpu)
{ return guest->handle_entry(vcpu); }

} // namespace

using namespace Vmm;
Entry vcpu_entries[64] =
{
  [0x00] = guest_unknown_fault,
  [0x01] = guest_wfx,
  [0x02] = guest_unknown_fault,
  [0x03] = guest_unknown_fault,
  [0x04] = guest_unknown_fault,
  [0x05] = guest_mcr_access,
  [0x06] = guest_unknown_fault,
  [0x07] = guest_unknown_fault,
  [0x08] = guest_unknown_fault,
  [0x09] = guest_unknown_fault,
  [0x0a] = guest_unknown_fault,
  [0x0b] = guest_unknown_fault,
  [0x0c] = guest_unknown_fault,
  [0x0d] = guest_unknown_fault,
  [0x0e] = guest_unknown_fault,
  [0x0f] = guest_unknown_fault,
  [0x10] = guest_unknown_fault,
  [0x11] = guest_unknown_fault,
  [0x12] = dispatch_vm_call,
  [0x13] = guest_unknown_fault,
  [0x14] = guest_unknown_fault,
  [0x15] = guest_unknown_fault,
  [0x16] = dispatch_vm_call,
  [0x17] = dispatch_smc,
  [0x18] = guest_msr_access,
  [0x19] = guest_unknown_fault,
  [0x1a] = guest_unknown_fault,
  [0x1b] = guest_unknown_fault,
  [0x1c] = guest_unknown_fault,
  [0x1d] = guest_unknown_fault,
  [0x1e] = guest_unknown_fault,
  [0x1f] = guest_unknown_fault,
  [0x20] = guest_memory_fault,
  [0x21] = guest_unknown_fault,
  [0x22] = guest_unknown_fault,
  [0x23] = guest_unknown_fault,
  [0x24] = guest_memory_fault,
  [0x25] = guest_unknown_fault,
  [0x26] = guest_unknown_fault,
  [0x27] = guest_unknown_fault,
  [0x28] = guest_unknown_fault,
  [0x29] = guest_unknown_fault,
  [0x2a] = guest_unknown_fault,
  [0x2b] = guest_unknown_fault,
  [0x2c] = guest_unknown_fault,
  [0x2d] = guest_unknown_fault,
  [0x2e] = guest_unknown_fault,
  [0x2f] = guest_unknown_fault,
  [0x30] = guest_unknown_fault,
  [0x31] = guest_unknown_fault,
  [0x32] = guest_unknown_fault,
  [0x33] = guest_unknown_fault,
  [0x34] = guest_unknown_fault,
  [0x35] = guest_unknown_fault,
  [0x36] = guest_unknown_fault,
  [0x37] = guest_unknown_fault,
  [0x38] = guest_unknown_fault,
  [0x39] = guest_unknown_fault,
  [0x3a] = guest_unknown_fault,
  [0x3b] = guest_unknown_fault,
  [0x3c] = guest_unknown_fault,
  [0x3d] = guest_ppi,
  [0x3e] = guest_unknown_fault,
  [0x3f] = guest_irq
};
