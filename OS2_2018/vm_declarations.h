#pragma once

#include <set>
#include <map>
#include "part.h"

typedef unsigned long PageNum;
typedef unsigned long VirtualAddress;
typedef void* PhysicalAddress;
typedef unsigned long Time;
typedef unsigned ProcessId;

typedef uint64_t pte_t;
typedef std::set<PhysicalAddress> BuddySystemLevel;
typedef BuddySystemLevel *BuddySystem;
typedef std::set<PhysicalAddress> PmtPool;

enum Status { OK, PAGE_FAULT, TRAP };
enum AccessType { READ = 1, WRITE, READ_WRITE, EXECUTE };
enum PTEMask { MASK_MAPPED = 0x200, MASK_LRU_DIRTY = 0x1f8, MASK_LRU = 0x1f0, MASK_ACCESSED = 0x100, MASK_ADD_BITS = 0x0f0, MASK_DIRTY = 0x008, MASK_FLAGS = 0x007 };

typedef struct RootClusterEntry {
	ProcessId pid;
	ClusterNo processCluster;
} RootClusterEntry;

typedef struct ProcessClusterEntry {
	VirtualAddress address;
	ClusterNo pageCluster;
} ProcessClusterEntry;

typedef struct REPC {
	ClusterNo rootCluster;
	unsigned rootEntry;
	ClusterNo processCluster;
} REPC;

typedef struct PEPC {
	ClusterNo processCluster;
	unsigned processEntry;
	ClusterNo pageCluster;
} PEPC;

typedef struct Segment {
	VirtualAddress startAddress;
	PageNum size;
	PageNum physicalSize;

	const bool operator< (const Segment& other) const {
		return startAddress < other.startAddress;
	}
} Segment;

typedef struct PTE {
	pte_t frame;
	bool mapped;
	bool accessed;
	uint8_t addBits;
	bool dirty;
	AccessType flags;
} PTE;

#define PAGE_OFFSET_LENGTH 10
#define PAGE_SIZE (1 << PAGE_OFFSET_LENGTH)

#define VIRTUAL_ADDRESS_LENGTH 24
#define PMT_SIZE (1 << (VIRTUAL_ADDRESS_LENGTH - PAGE_OFFSET_LENGTH))
#define SIZE_OF_PMT_IN_PAGES ((PMT_SIZE * sizeof(pte_t) - 1) / PAGE_SIZE + 1)

#define PTE_FRAME_SHIFT 10
#define PTE_ADD_BITS_SHIFT 4

#define ROOT_CLUSTER_ENTRIES (ClusterSize / sizeof(RootClusterEntry))
#define PROCESS_CLUSTER_ENTRIES (ClusterSize / sizeof(ProcessClusterEntry))
