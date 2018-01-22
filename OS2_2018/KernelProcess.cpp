#include "KernelProcess.h"
#include "KernelSystem.h"

KernelProcess::KernelProcess(ProcessId pid) {
	this->pid = pid;
}

KernelProcess::~KernelProcess() {
	while (!segments.empty()) {
		auto s = segments.begin();
		deleteSegment(s->first);
	}
	pSystem->eraseProcessFromPartition_s(pid);
	pSystem->giveToPmtPool_s((PhysicalAddress)pmt);
	//pSystem->printPmtPoolTop();
}

ProcessId KernelProcess::getProcessId() const {
	return pid;
}

Status KernelProcess::createSegment(VirtualAddress startAddress, PageNum segmentSize,
		AccessType flags) {
	if (startAddress % PAGE_SIZE) {
		return TRAP;
	}

	Segment* s = new Segment();
	s->startAddress = startAddress;
	s->size = segmentSize;
	s->physicalSize = 0;
	segments[startAddress] = s;

	auto currSegment = segments.find(startAddress),
		prevSegment = currSegment,
		nextSegment = currSegment;
	prevSegment--;
	nextSegment++;
	if (((prevSegment != segments.end()) && (prevSegment->second->startAddress + prevSegment->second->size * PAGE_SIZE > currSegment->second->startAddress))
			|| ((nextSegment != segments.end()) && (currSegment->second->startAddress + currSegment->second->size * PAGE_SIZE > nextSegment->second->startAddress))) {
		segments.erase(currSegment);
		return TRAP;
	}

	for (PageNum currentPage = 0; currentPage < segmentSize; currentPage++) {
		VirtualAddress currentAddress = startAddress + currentPage * PAGE_SIZE;
		PTE entry;
		getPTE(currentAddress, &entry);
		if (entry.mapped) {
			segments.erase(currSegment);
			return TRAP;
		}
		entry.frame = 0;
		entry.mapped = true;
		entry.accessed = false;
		entry.dirty = false;
		entry.flags = flags;
		putPTE(currentAddress, entry);
	}

	//printSegmentsTop();
	//printPmtFromAddress(startAddress);
	//printf("Done creating segment\n");
	return OK;
}

Status KernelProcess::loadSegment(VirtualAddress startAddress, PageNum segmentSize,
		AccessType flags, void* content) {
	Status retVal = createSegment(startAddress, segmentSize, flags);
	if (retVal == OK) {
		pSystem->writeToPartition_s(pid, startAddress, segmentSize, content);
	}
	return retVal;
}

Status KernelProcess::deleteSegment(VirtualAddress startAddress) {
	if (startAddress % PAGE_SIZE) {
		return TRAP;
	}
	auto s = segments.find(startAddress);
	if (s == segments.end()) {
		return TRAP;
	}

	PageNum segmentSize = s->second->size;
	for (PageNum currentPage = 0; currentPage < segmentSize; currentPage++) {
		VirtualAddress currentAddress = startAddress + currentPage * PAGE_SIZE;
		PTE pte;
		getPTE(currentAddress, &pte);
		if (pte.frame) {
			pSystem->giveToBuddySystem_s(getPhysicalAddress(currentAddress), 1);
		} else {
			pSystem->erasePageFromPartition_s(pid, currentAddress);
		}
		pte.frame = 0;
		putPTE(currentAddress, pte);
	}

	pSystem->defragmentBuddySystem_s();
	segments.erase(s);

	//printSegmentsTop();
	//printPmtFromAddress(startAddress);
	//printf("Done deleting segment\n");
	return OK;
}

Status KernelProcess::pageFault(VirtualAddress address) {
	if (!address) {
		return TRAP;
	}
	VirtualAddress pageAddress = address & ~PAGE_OFFSET_MASK;
	PTE pte;
	getPTE(pageAddress, &pte);
	if (!pte.mapped) {
		return TRAP;
	}
	PhysicalAddress frameAddress = pSystem->takeFromBuddySystem_s(1);
	if (!frameAddress) {
		frameAddress = pSystem->ejectPageAndGetFrame_s();
		pSystem->loadFromPartition_s(pid, pageAddress, frameAddress);
	}
	pte.frame = (uint64_t)frameAddress >> PAGE_OFFSET_LENGTH;
	pte.accessed = false;
	pte.dirty = false;
	putPTE(pageAddress, pte);

	Segment* found = 0;
	for (auto s : segments) {
		if ((address >= s.second->startAddress) && (address < s.second->startAddress + s.second->size * PAGE_SIZE)) {
			found = s.second;
			break;
		}
	}
	if (!found) {
		printf("Couldn't find segment to which the virtual address %06lu belongs\n", address);
		throw std::exception();
	}
	found->physicalSize++;

	//PageNum physicalMemory = getTotalPhysicalMemory();
	//PageNum actualPhysicalMemory = getActualPhysicalMemory();
	//if (physicalMemory != actualPhysicalMemory) {
	//	printf("Starting to differ!\n");
	//}

	//printPmtFromAddress(address);
	//printSegmentsTop();
	//printf("Done page fault\n");
	return OK;
}

PhysicalAddress KernelProcess::getPhysicalAddress(VirtualAddress address) {
	if (!address) {
		return 0;
	}
	pte_t entry = *getEntryForAddress(address);
	if (!(entry >> PTE_FRAME_SHIFT) || !(entry & MASK_MAPPED)) {
		return 0;
	}
	pte_t frameAddress = (entry >> PTE_FRAME_SHIFT) << PAGE_OFFSET_LENGTH;
	unsigned offset = address % PAGE_SIZE;
	return (PhysicalAddress)(frameAddress + offset);
}

void KernelProcess::initialize(KernelSystem* pSystem) {
	this->pSystem = pSystem;

	// init pmt
	pmt = (pte_t*)pSystem->takeFromPmtPool_s();
	if (!pmt) {
		printf("Cannot create process %u, no space left in PMT pool\n", this->pid);
		throw std::exception();
	}
	pSystem->printPmtPoolTop();
}

pte_t* KernelProcess::getEntryForAddress(VirtualAddress address) {
	PageNum entryNumber = address / PAGE_SIZE;
	return &(pmt[entryNumber]);
}

void KernelProcess::getPTE(VirtualAddress address, PTE* pte) {
	pte_t entry = *getEntryForAddress(address);
	pte->frame = entry >> PTE_FRAME_SHIFT;
	pte->mapped = entry & MASK_MAPPED;
	pte->accessed = entry & MASK_ACCESSED;
	pte->dirty = entry & MASK_DIRTY;
	pte->flags = (AccessType)(entry & MASK_FLAGS);
}

void KernelProcess::putPTE(VirtualAddress address, PTE pte) {
	pte_t* entry = getEntryForAddress(address);
	*entry = pte.frame << PTE_FRAME_SHIFT;
	if (pte.mapped) *entry = *entry | MASK_MAPPED;
	if (pte.accessed) *entry = *entry | MASK_ACCESSED;
	if (pte.dirty) *entry = *entry | MASK_DIRTY;
	*entry = *entry | pte.flags;
}

Status KernelProcess::accessPTE(VirtualAddress address, AccessType type) {
	pte_t* entry = getEntryForAddress(address);
	if (!(*entry >> PTE_FRAME_SHIFT) || !(*entry & MASK_MAPPED)) {
		return PAGE_FAULT;
	}
	*entry = *entry | MASK_ACCESSED;
	if (type & WRITE) {
		*entry = *entry | MASK_DIRTY;
	}
	return OK;
}

PhysicalAddress KernelProcess::ejectPageAndGetFrame_s() {
	for (PageNum i = 0; i < (PMT_SIZE << 1); i++) {
		pte_t* entry = &(pmt[clockHand]);
		PageNum prevClockHand = clockHand;
		clockHand = (clockHand + 1) % PMT_SIZE;
		if (!(*entry >> PTE_FRAME_SHIFT)) {
			// don't bother, it's not in physical memory
			continue;
		} else if (*entry & MASK_ACCESSED) {
			// just reset the accessed bit
			*entry = *entry & (~MASK_ACCESSED);
		} else {
			//printf("Tried %lu entries before ejecting\n", i);
			// we have got our victim
			VirtualAddress virtualAddress = prevClockHand << PAGE_OFFSET_LENGTH;
			PTE pte;
			getPTE(virtualAddress, &pte);
			PhysicalAddress physicalAddress = (PhysicalAddress)(pte.frame << PAGE_OFFSET_LENGTH);
			if (pte.dirty) {
				// first write to disk
				pSystem->writeToPartition(pid, virtualAddress, 1, physicalAddress);
				pte.dirty = false;
			}
			// remove the frame from pmt
			pte.frame = 0;
			pte.accessed = false;
			putPTE(virtualAddress, pte);
			// remove the physical space from segment
			Segment* found = 0;
			for (auto s : segments) {
				if ((virtualAddress >= s.second->startAddress) && (virtualAddress < s.second->startAddress + (s.second->size << PAGE_OFFSET_LENGTH))) {
					found = s.second;
					break;
				}
			}
			if (!found) {
				printf("Couldn't find segment to which the virtual address %06lu belongs\n", virtualAddress);
				throw std::exception();
			}
			found->physicalSize--;
			//printf("Done ejecting page\n");
			return physicalAddress;
		}
	}
	return 0;
}

PageNum KernelProcess::getTotalPhysicalMemory() {
	PageNum retVal = 0;
	for (auto s : segments) {
		retVal += s.second->physicalSize;
	}
	return retVal;
}

PageNum KernelProcess::getTotalVirtualMemory() {
	PageNum retVal = 0;
	for (auto s : segments) {
		retVal += s.second->size;
	}
	return retVal;
}

void KernelProcess::printSegmentsTop() {
	printf("\n +========== SEGMENTS TOP ==========\n");
	int i = 0;
	for (auto s : segments) {
		if (i++ >= 5) {
			break;
		}
		printf(" | %06lx | %06lu | %06lu", s.second->startAddress, s.second->size, s.second->physicalSize);
		printf("\n +----------------------------------\n");
	}
}

void KernelProcess::printPmtFromAddress(VirtualAddress address) {
	PTE entry;
	printf("\n +========== PMT TOP ==========\n");
	printf(" | VA     | frame      | mapped | accessed | dirty | flags");
	printf("\n +-----------------------------\n");
	for (PageNum page = 0; page < 5; page++) {
		VirtualAddress currentAddress = address + page * PAGE_SIZE;
		getPTE(currentAddress, &entry);
		printf(" | %06lx | 0x%08llx | %d      | %d        | %d     | %d", currentAddress, entry.frame, entry.mapped, entry.accessed, entry.dirty, entry.flags);
		printf("\n +-----------------------------\n");
	}
}

PageNum KernelProcess::getActualPhysicalMemory() {
	PageNum retVal = 0;
	for (PageNum page = 0; page < PMT_SIZE; page++) {
		PTE pte;
		getPTE(page * PAGE_SIZE, &pte);
		if (pte.mapped && pte.frame) {
			retVal++;
		}
	}
	return retVal;
}

void KernelProcess::printPmtStats() {
	PageNum accessedCount = 0;
	PageNum lowAddBitsCount = 0;
	PageNum highAddBitsCount = 0;
	PageNum dirtyCount = 0;
	PageNum inMemoryCount = 1;
	PageNum mappedCount = 1;
	for (PageNum page = 0; page < PMT_SIZE; page++) {
		PTE pte;
		getPTE(page * PAGE_SIZE, &pte);
		if (pte.mapped) mappedCount++;
		if (pte.mapped && pte.frame) inMemoryCount++;
		if (pte.mapped && pte.frame && pte.accessed) accessedCount++;
		if (pte.mapped && pte.frame && pte.dirty) dirtyCount++;
	}
	printf("Number of mapped pages for process %lu : %lu\n", pid, mappedCount);
	printf("Ratio of in-memory to mapped pages for process %lu : %f\n", pid, (double)inMemoryCount / mappedCount);
	printf("Ratio of accessed to in-memory pages for process %lu : %f\n", pid, (double)accessedCount / inMemoryCount);
	printf("Ratio of dirty to in-memory pages for process %lu : %f\n", pid, (double)dirtyCount / inMemoryCount);
}
