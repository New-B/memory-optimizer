#include <stdio.h>
#include <map>
#include <string>
#include <iostream>
#include <algorithm>
#include <sys/mman.h>

#include <numa.h>
#include <numaif.h>
#include "Migration.h"
#include "lib/debug.h"

using namespace std;

Migration::Migration(ProcIdlePages& pip)
  : proc_idle_pages(pip)
{
  memset(&policies, 0, sizeof(policies));
}

int Migration::select_top_pages(ProcIdlePageType type)
{
  const page_refs_map& page_refs = proc_idle_pages.get_pagetype_refs(type).page_refs;
  int nr_walks = proc_idle_pages.get_nr_walks();
  int nr_pages;

  nr_pages = page_refs.size();
  cout << "nr_pages: " << nr_pages << endl;

  for (auto it = page_refs.begin(); it != page_refs.end(); ++it) {
    printdd("vpfn: %lx count: %d\n", it->first, (int)it->second);

    if (it->second >= nr_walks)
      pages_addr[type].push_back((void *)(it->first << PAGE_SHIFT));
  }

  sort(pages_addr[type].begin(), pages_addr[type].end());

  if (debug_level() >= 2)
    for (int i = 0; i < nr_pages; ++i) {
      cout << "page " << i << ": " << pages_addr[type][i] << endl;
    }

  return 0;
}

int Migration::set_policy(int samples_percent, int pages_percent,
                          int node, ProcIdlePageType type)
{
  policies[type].nr_samples_percent = samples_percent;
  policies[type].nr_pages_percent = pages_percent;
  policies[type].node = node;

  return 0;
}

int Migration::locate_numa_pages(ProcIdlePageType type)
{
  pid_t pid = proc_idle_pages.get_pid();
  vector<void *>::iterator it;
  int ret;

  auto& params = policies[type];
  auto& addrs = pages_addr[type];

  int nr_pages = addrs.size();
  migrate_status.resize(nr_pages);
  ret = move_pages(pid, nr_pages, &addrs[0], NULL,
                   &migrate_status[0], MPOL_MF_MOVE);
  if (ret) {
    perror("move_pages");
    return ret;
  }

  int i, j;
  for (i = 0, j = 0; i < nr_pages; ++i) {
    if (migrate_status[i] >= 0 &&
        migrate_status[i] != params.node)
      addrs[j++] = addrs[i];
  }

  addrs.resize(j);

  return 0;
}

int Migration::migrate(ProcIdlePageType type)
{
  pid_t pid = proc_idle_pages.get_pid();
  std::vector<int> nodes;

  int ret;

  ret = select_top_pages(type);
  if (ret)
    return ret;

  ret = locate_numa_pages(type);
  if (ret)
    return ret;

  auto& params = policies[type];
  auto& addrs = pages_addr[type];

  int nr_pages = addrs.size();
  cout << "nr_pages: " << nr_pages << endl;

  migrate_status.resize(nr_pages);
  nodes.resize(nr_pages, params.node);
  ret = move_pages(pid, nr_pages, &addrs[0], &nodes[0],
                   &migrate_status[0], MPOL_MF_MOVE);
  if (ret) {
    perror("move_pages");
    return ret;
  }

  return ret;
}