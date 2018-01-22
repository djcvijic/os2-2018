#pragma once

#include <mutex>
#include "vm_declarations.h"

class Partition;
class System;

typedef std::map<ProcessId, Process*> ProcessMap;

class KernelSystem {
public:
	KernelSystem(PhysicalAddress processVMSpace, PageNum processVMSpaceSize,
		PhysicalAddress pmtSpace, PageNum pmtSpaceSize,
		Partition* partition, System* system);
	~KernelSystem();
	Process* createProcess();
	Time periodicJob();
	// Hardware job
	Status access(ProcessId pid, VirtualAddress address, AccessType type);

	static bool firstEjectHappened;
private:
	PhysicalAddress processVMSpace;
	PageNum processVMSpaceSize;
	PhysicalAddress pmtSpace;
	PageNum pmtSpaceSize;
	Partition* partition;
	System* system;

	std::mutex _mutex;
	ClusterNo numOfClusters;
	ClusterNo freeClusterList = 1;
	BuddySystem buddySystem;
	int buddySystemLevelCount;
	PmtPool pmtPool;
	ProcessMap processMap;
	ProcessId nextPid = 1;
	ProcessId processClockHand = 0;
	KernelProcess* victimProcess = 0;

	ClusterNo rootClusterCount = 1;
	ClusterNo processClusterCount = 0;
	ClusterNo pageClusterCount = 0;

	ClusterNo getNextFreeCluster();
	void getProcessCluster(ProcessId pid, REPC* ret);
	void getPageCluster(ClusterNo processCluster, VirtualAddress address, PEPC* ret);
	void writeToPartition(ProcessId pid, VirtualAddress startAddress, PageNum pageCount, void* content);
	void writeToPartition_s(ProcessId pid, VirtualAddress startAddress, PageNum pageCount, void* content);
	void erasePageFromPartition_s(ProcessId pid, VirtualAddress address);
	void eraseProcessFromPartition_s(ProcessId pid);
	void loadFromPartition_s(ProcessId pid, VirtualAddress virtualAddress, PhysicalAddress physicalAddress);
	void calculateVictimProcess();
	PhysicalAddress ejectPageAndGetFrame_s();
	PageNum getTotalVirtualMemory();
	void printFreeClustersTop();
	void printRootClusterTop();
	void printProcessClusterTop(ProcessId pid);
	void printPageClusterTop(ProcessId pid, VirtualAddress address);

	void giveToBuddySystem(PhysicalAddress startAddress, PageNum pageCount);
	void giveToBuddySystem_s(PhysicalAddress startAddress, PageNum pageCount);
	PhysicalAddress takeFromBuddySystem_s(PageNum pageCount);
	void defragmentBuddySystem();
	void defragmentBuddySystem_s();
	void printBuddySystem();

	void giveToPmtPool(PhysicalAddress address);
	void giveToPmtPool_s(PhysicalAddress address);
	PhysicalAddress takeFromPmtPool_s();
	void printPmtPoolTop();

	friend class KernelProcess;
};
