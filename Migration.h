#ifndef _MIGRATION_H
#define _MIGRATION_H

/*
 * The header for migrating pages.
 */

#include <sys/types.h>

#include <unordered_map>
#include <string>
#include <vector>

#include "Option.h"
#include "Formatter.h"
#include "ProcVmstat.h"
#include "ProcIdlePages.h"

class Migration : public ProcIdlePages
{
  public:
    // functions
    Migration(pid_t n);
    ~Migration() {};

    // migrate pages to nodes
    int migrate();
    int migrate(ProcIdlePageType type);

    void set_policy(Policy &policy);

    int dump_task_nodes();
    int dump_vma_nodes(proc_maps_entry& vma);

    static void show_numa_stats();

 private:
    // functions

    size_t get_threshold_refs(ProcIdlePageType type, int& min_refs, int& max_refs);

    // select max counted pages in page_refs_4k and page_refs_2m
    int select_top_pages(ProcIdlePageType type);

    void fill_addrs(std::vector<void *>& addrs, unsigned long start);
    void dump_node_percent();

    long __move_pages(pid_t pid, unsigned long nr_pages, void **addrs, int node);
    long do_move_pages(ProcIdlePageType type);

    // status => count
    std::unordered_map<int, int> calc_migrate_stats();

  private:
    // The Virtual Address of hot/cold pages.
    // [0...n] = [VA0...VAn]
    //std::vector<unsigned long> hot_pages;
    std::vector<void *> pages_addr[PMD_ACCESSED + 1];

    std::vector<int> migrate_target_node;

    // Get the status after migration
    std::vector<int> migrate_status;

    Formatter fmt;

    MigrateWhat migrate_what;
    bool dump_distribution;
};

#endif
// vim:set ts=2 sw=2 et:
