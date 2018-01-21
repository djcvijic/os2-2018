#pragma once

#include "vm_declarations.h"

class Process;
class KernelSystem;

class KernelProcess {
public:
	KernelProcess(ProcessId pid);
	~KernelProcess();
	ProcessId getProcessId() const;
	Status createSegment(VirtualAddress startAddress, PageNum segmentSize,
		AccessType flags);
	Status loadSegment(VirtualAddress startAddress, PageNum segmentSize,
		AccessType flags, void* content);
	Status deleteSegment(VirtualAddress startAddress);
	Status pageFault(VirtualAddress address);
	PhysicalAddress getPhysicalAddress(VirtualAddress address);
private:
	ProcessId pid;
	KernelSystem* pSystem;
	Process* process;

	std::map<VirtualAddress, Segment*> segments;
	pte_t* pmt;
	PageNum clockHand = 0;

	void initialize(KernelSystem* pSystem);
	pte_t* getEntryForAddress(VirtualAddress address);
	void getPTE(VirtualAddress address, PTE* pte);
	void putPTE(VirtualAddress address, PTE pte);
	Status accessPTE(VirtualAddress address, AccessType type);
	PhysicalAddress ejectPageAndGetFrame_s();
	PageNum getTotalPhysicalMemory();
	PageNum getTotalVirtualMemory();
	void printSegmentsTop();
	void printPmtFromAddress(VirtualAddress address);

	PageNum getActualPhysicalMemory();
	void printPmtStats();

	friend class KernelSystem;
};
