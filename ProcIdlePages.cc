#include <fcntl.h>
#include <iostream>
#include <linux/limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "Option.h"
#include "ProcIdlePages.h"
#include "lib/debug.h"

extern Option option;

int pagetype_index[] = {
  [PTE_ACCESSED]      = PTE_ACCESSED,
  [PMD_ACCESSED]      = PMD_ACCESSED,
  [PUD_PRESENT]       = PUD_PRESENT,

  [PTE_DIRTY]         = PTE_ACCESSED,
  [PMD_DIRTY]         = PMD_ACCESSED,

  [PTE_IDLE]          = PTE_ACCESSED,
  [PMD_IDLE]          = PMD_ACCESSED,
};

unsigned long pagetype_size[IDLE_PAGE_TYPE_MAX] = {
  [PTE_ACCESSED]      = PAGE_SIZE,
  [PMD_ACCESSED]      = PMD_SIZE,
  [PUD_PRESENT]       = PUD_SIZE,

  [PTE_DIRTY]         = PAGE_SIZE,
  [PMD_DIRTY]         = PMD_SIZE,

  [PTE_IDLE]          = PAGE_SIZE,
  [PMD_IDLE]          = PMD_SIZE,
  [PMD_IDLE_PTES]     = PMD_SIZE,

  [PTE_HOLE]          = PAGE_SIZE,
  [PMD_HOLE]          = PMD_SIZE,
  [PUD_HOLE]          = PUD_SIZE,
  [P4D_HOLE]          = P4D_SIZE,
  [PGDIR_HOLE]        = PGDIR_SIZE,
};

int pagetype_shift[IDLE_PAGE_TYPE_MAX] = {
  [PTE_ACCESSED]      = 12,
  [PMD_ACCESSED]      = 21,
  [PUD_PRESENT]       = 30,

  [PTE_DIRTY]         = 12,
  [PMD_DIRTY]         = 21,

  [PTE_IDLE]          = 12,
  [PMD_IDLE]          = 21,
  [PMD_IDLE_PTES]     = 21,

  [PTE_HOLE]          = 12,
  [PMD_HOLE]          = 21,
  [PUD_HOLE]          = 30,
  [P4D_HOLE]          = 39,
  [PGDIR_HOLE]        = 39,
};


const char* pagetype_name[IDLE_PAGE_TYPE_MAX] = {
  [PTE_ACCESSED]      = "4K_accessed",
  [PMD_ACCESSED]      = "2M_accessed",
  [PUD_PRESENT]       = "1G_present",

  [PTE_DIRTY]         = "4K_dirty",
  [PMD_DIRTY]         = "2M_dirty",

  [PTE_IDLE]          = "4K_idle",
  [PMD_IDLE]          = "2M_idle",
  [PMD_IDLE_PTES]     = "2M_idle_4ks",

  [PTE_HOLE]          = "4K_hole",
  [PMD_HOLE]          = "2M_hole",
  [PUD_HOLE]          = "1G_hole",
  [P4D_HOLE]          = "512G_hole",
  [PGDIR_HOLE]        = "512G_hole",
};

ProcIdlePages::ProcIdlePages()
{
  va_start = 0;
  va_end = TASK_SIZE_MAX;
  io_error = 0;
}

int ProcIdlePages::walk_vma(proc_maps_entry& vma)
{
  unsigned long va = vma.start;
  unsigned long end = vma.end;
  unsigned long size;
  int rc = 0;

  if (end <= va_start)
    return 0;

  // skip [vsyscall] etc. special kernel sections
  if (va >= va_end)
    return 1; // stop walking more vma

  if (!proc_maps.is_anonymous(vma))
    return 0;

  if (debug_level() >= 2)
    proc_maps.show(vma);

  if (va < va_start)
    va = va_start;
  if (end > va_end)
    end = va_end;

  if (lseek(idle_fd, va_to_offset(va), SEEK_SET) == (off_t) -1)
  {
    printf(" error: seek for addr %lx failed, skip.\n", va);
    perror("lseek error");
    io_error = -1;
    return -1;
  }

  for (; va < end;)
  {
    off_t pos = lseek(idle_fd, 0, SEEK_CUR);
    if (pos == (off_t) -1) {
      perror("SEEK_CUR error");
      io_error = -1;
      return -1;
    }
    if ((unsigned long)pos != va) {
      fprintf(stderr, "error va-pos != 0: %lx-%lx=%lx\n", va, pos, va - pos);

      if (va > (unsigned long)pos)
        if (lseek(idle_fd, va_to_offset(va), SEEK_SET) == (off_t) -1)
        {
          printf(" error: seek for addr %lx failed, skip.\n", va);
          perror("lseek error");
          io_error = -1;
          return -2;
        }
    }

    size = (end - va + (7 << PAGE_SHIFT)) >> (3 + PAGE_SHIFT);
    if (size > read_buf.size())
      size = read_buf.size();

    rc = read(idle_fd, read_buf.data(), size);
    if (rc < 0) {
      if (errno == ENXIO)
        return 0;
      if (errno == ERANGE) {
        va += size << (3 + PAGE_SHIFT);
        pos = lseek(idle_fd, va_to_offset(va), SEEK_SET);
        if (pos == (off_t) -1) {
          perror("skip ERANGE");
          io_error = -1;
          return -1;
        }
        continue;
      }
      perror("read error");
      proc_maps.show(vma);
      io_error = rc;
      return rc;
    }

    if (!rc)
    {
      if (end - va >= PMD_SIZE) {
        printf("read 0 size: pid=%d bytes=%'lu\n", pid, end - va);
        proc_maps.show(vma);
      }
      return 0;
    }

    parse_idlepages(vma, va, end, rc);
  }

  return 0;
}

int ProcIdlePages::walk()
{
  // Assume PLACEMENT_DRAM processes will mlock themselves to LRU_UNEVICTABLE.
  // Just need to skip them in user space migration.
  if (policy.placement == PLACEMENT_DRAM) {
    io_error = 1;
    return 0;
  }

  std::vector<proc_maps_entry> address_map = proc_maps.load(pid);
  int err = -1;

  if (address_map.empty()) {
    io_error = -ESRCH;
    return -ESRCH;
  }

  idle_fd = open_file();
  if (idle_fd < 0)
    return idle_fd;

  ++nr_walks;
  read_buf.resize(READ_BUF_SIZE);

  // must do rewind() before a walk() start.
  for (auto& prc: pagetype_refs)
    prc.page_refs.rewind();

  for (auto &vma: address_map) {
    err = walk_vma(vma);
    if (err)
      break;
  }

  close(idle_fd);

  return err;
}

int ProcIdlePages::open_file()
{
  char filepath[PATH_MAX];

  memset(filepath, 0, sizeof(filepath));
  snprintf(filepath, sizeof(filepath), "/proc/%d/idle_bitmap", pid);

  idle_fd = open(filepath, O_RDWR);
  if (idle_fd < 0) {
    io_error = idle_fd;
    perror(filepath);
  }

  return idle_fd;
}

void ProcIdlePages::inc_page_refs(ProcIdlePageType type, int nr,
                                  unsigned long va, unsigned long end)
{
  unsigned long page_size = pagetype_size[type];
  AddrSequence& page_refs = pagetype_refs[pagetype_index[type]].page_refs;

  if (va & (page_size - 1)) {
    printf("ignore unaligned addr: %d %lx+%d %lx\n", type, va, nr, page_size);
    return;
  }

  for (int i = 0; i < nr; ++i)
  {
    if (type >= PTE_IDLE)
      page_refs.inc_payload(va, 0);
    else if (type >= PTE_DIRTY)
      page_refs.inc_payload(va, 3);
    else // accessed
      page_refs.inc_payload(va, 1);

    //AddrSequence is not easy to random access, consider move
    //this checking into AddrSequence.
    //if (page_refs[va] > nr_walks)
    //  printf("error counted duplicate va: %lx\n", va);

    va += page_size;
    if (va >= end)
      break;
  }
}

void ProcIdlePages::dump_idlepages(proc_maps_entry& vma, int bytes)
{
  proc_maps.show(vma);
  for (int j = 0; j < bytes; ++j)
    printf("%x:%x  ", read_buf[j].type, read_buf[j].nr);
  puts("");
}

void ProcIdlePages::parse_idlepages(proc_maps_entry& vma,
                                    unsigned long& va,
                                    unsigned long end,
                                    int bytes)
{
  int dumped = 0;

  for (int i = 0; i < bytes; ++i)
  {
    ProcIdlePageType type = (ProcIdlePageType) read_buf[i].type;
    int nr = read_buf[i].nr;

    if (va >= end) {
      // This can happen infrequently when VMA changed. The new pages can be
      // simply ignored -- they arrive too late to have accurate accounting.
      if (debug_level() >= 2) {
        printf("WARNING: va >= end: %lx %lx i=%d bytes=%d type=%d nr=%d\n",
               va, end, i, bytes, type, nr);
        if (dumped++ == 0)
          dump_idlepages(vma, bytes);
      }
      return;
    }

    if (debug_level() >= 2) {
      unsigned long align = va & (pagetype_size[type] - 1);
      if (align) {
        printf("align va %lx-%lx @%d %x:%x\n", va, align, i, type, nr);
        if (dumped++ == 0)
          dump_idlepages(vma, bytes);
      }
    }

    va &= ~(pagetype_size[type] - 1);

    if (type <= MAX_ACCESSED) {
      if (type == PMD_IDLE_PTES)
        inc_page_refs(PTE_IDLE, nr * 512, va, end);
      else
        inc_page_refs(type, nr, va, end);
    }

    va += pagetype_size[type] * nr;
  }
}

unsigned long ProcIdlePages::va_to_offset(unsigned long va)
{
  return va;
}

unsigned long ProcIdlePages::offset_to_va(unsigned long offset)
{
  return offset;
}

void ProcIdlePages::set_va_range(unsigned long start, unsigned long end)
{
  va_start = start;
  va_end = end;
}

void ProcIdlePages::set_policy(Policy &pol)
{
  policy = pol;
}
