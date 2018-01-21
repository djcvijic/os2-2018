#include "part.h"
#include "Process.h"
#include "KernelProcess.h"
#include "KernelSystem.h"

KernelSystem::KernelSystem(PhysicalAddress processVMSpace, PageNum processVMSpaceSize,
		PhysicalAddress pmtSpace, PageNum pmtSpaceSize,
		Partition* partition, System* system) {
	firstEjectHappened = false;

	if (PAGE_SIZE != ClusterSize) {
		printf("Cannot start KernelSystem because PAGE_SIZE (%lu) differs from ClusterSize (%lu)\n", PAGE_SIZE, ClusterSize);
		throw std::exception();
	}

	this->processVMSpace = processVMSpace;
	this->processVMSpaceSize = processVMSpaceSize;
	this->pmtSpace = pmtSpace;
	this->pmtSpaceSize = pmtSpaceSize;
	this->partition = partition;
	this->system = system;

	// init partition
	numOfClusters = partition->getNumOfClusters();
	char buffer[ClusterSize];
	memset(buffer, 0, ClusterSize);
	partition->writeCluster(0, buffer);
	for (ClusterNo c = 1; c < numOfClusters; c++) {
		*((ClusterNo*)buffer) = (c + 1) % numOfClusters;
		partition->writeCluster(c, buffer);
	}
	freeClusterList = 1;

	// init buddy system
	buddySystemLevelCount = 0;
	for (PageNum tempBuddySpaceSize = processVMSpaceSize; tempBuddySpaceSize; tempBuddySpaceSize >>= 1) {
		buddySystemLevelCount++;
	}
	buddySystem = new BuddySystemLevel[buddySystemLevelCount];
	memset(processVMSpace, 0, processVMSpaceSize * PAGE_SIZE);
	giveToBuddySystem(processVMSpace, processVMSpaceSize);
	printBuddySystem();

	// init pmt pool
	memset(pmtSpace, 0, pmtSpaceSize * PAGE_SIZE);
	PhysicalAddress currentPmtAddress = pmtSpace;
	for (PageNum currentPmtPage = 0; currentPmtPage < pmtSpaceSize; currentPmtPage += SIZE_OF_PMT_IN_PAGES) {
		giveToPmtPool(currentPmtAddress);
		currentPmtAddress = (PhysicalAddress)((uint64_t)currentPmtAddress + SIZE_OF_PMT_IN_PAGES * PAGE_SIZE);
	}
	printPmtPoolTop();

	// test buddy system
	//printBuddySystem();
	//auto firstChunk = takeFromBuddySystem(4096);
	//printBuddySystem();
	//auto secondChunk = takeFromBuddySystem(128);
	//printBuddySystem();
	//giveToBuddySystem(secondChunk, 128);
	//printBuddySystem();
	//giveToBuddySystem(firstChunk, 4096);
	//printBuddySystem();
	//defragmentBuddySystem();
	//printBuddySystem();
}

KernelSystem::~KernelSystem() {
	delete[] buddySystem;
}

Process* KernelSystem::createProcess() {
	Process* p = new Process(nextPid);
	p->pProcess->initialize(this);
	processMap[nextPid] = p;
	nextPid++;
	return p;
}

Time KernelSystem::periodicJob() {
	// TODO: clock algorithm tick (maybe return zero if this part fails), maybe defragment buddy system,
	// maybe write dirty pages to partition, maybe swap in some absent stuff, maybe swap out if free space is low...

	// return the tick length value (should it ever change?)
	return 18000;
}

Status KernelSystem::access(ProcessId pid, VirtualAddress address, AccessType type) {
	if (!address) {
		// Disallow address zero
		return TRAP;
	}
	ProcessMap::iterator processIter = processMap.find(pid);
	if (processIter == processMap.end()) {
		// The process does not exist
		return TRAP;
	}
	KernelProcess* kp = processIter->second->pProcess;
	PTE entry;
	kp->getPTE(address, &entry);
	if (!entry.mapped || !(entry.flags & type)) {
		// The page is not mapped, or access type is incorrect
		return TRAP;
	}
	if (!entry.frame) {
		// The page is not present
		return PAGE_FAULT;
	}
	kp->accessPTE(address, type);
	return OK;
}

ClusterNo KernelSystem::getNextFreeCluster() {
	if (!freeClusterList) {
		printf("Free cluster list points to zero cluster, which means no free clusters remain!\n");
		throw std::exception();
	}
	ClusterNo nextFreeCluster = freeClusterList;
	char buffer[ClusterSize];
	partition->readCluster(freeClusterList, buffer);
	freeClusterList = *((ClusterNo*)buffer);
	//printFreeClustersTop();
	return nextFreeCluster;
}

void KernelSystem::getProcessCluster(ProcessId pid, REPC* ret) {
	ClusterNo prevRootCluster = ret->rootCluster = 0;
	char rootBuffer[ClusterSize];
	do {
		partition->readCluster(ret->rootCluster, rootBuffer);
		for (ret->rootEntry = 1; ret->rootEntry < ROOT_CLUSTER_ENTRIES; ret->rootEntry++) {
			RootClusterEntry* entry = (RootClusterEntry*)(rootBuffer + ret->rootEntry * sizeof(RootClusterEntry));
			if (entry->pid == pid) {
				// found it yay!
				ret->processCluster = entry->processCluster;
				return;
			}
			if (entry->pid == 0) {
				// got to the end without finding the pid, create a new process cluster
				ret->processCluster = getNextFreeCluster();
				char processBuffer[ClusterSize];
				memset(processBuffer, 0, ClusterSize);
				partition->writeCluster(ret->processCluster, processBuffer);

				entry->pid = pid;
				entry->processCluster = ret->processCluster;
				partition->writeCluster(ret->rootCluster, rootBuffer);

				return;
			}
		}
		prevRootCluster = ret->rootCluster;
		ret->rootCluster = *((ClusterNo*)rootBuffer);
	} while (ret->rootCluster);

	// got to the end, haven't found it, and need to create a new root cluster first, then new process cluster *sigh*
	ret->rootCluster = getNextFreeCluster();
	*((ClusterNo*)rootBuffer) = ret->rootCluster;
	partition->writeCluster(prevRootCluster, rootBuffer);

	ret->processCluster = getNextFreeCluster();
	char processBuffer[ClusterSize];
	memset(processBuffer, 0, ClusterSize);
	partition->writeCluster(ret->processCluster, processBuffer);

	memset(rootBuffer, 0, ClusterSize);
	ret->rootEntry = 1;
	RootClusterEntry* entry = (RootClusterEntry*)(rootBuffer + sizeof(RootClusterEntry));
	entry->pid = pid;
	entry->processCluster = ret->processCluster;
	partition->writeCluster(ret->rootCluster, rootBuffer);
}

void KernelSystem::getPageCluster(ClusterNo processCluster, VirtualAddress address, PEPC* ret) {
	ClusterNo prevProcessCluster = ret->processCluster = processCluster;
	char processBuffer[ClusterSize];
	do {
		partition->readCluster(ret->processCluster, processBuffer);
		for (ret->processEntry = 1; ret->processEntry < PROCESS_CLUSTER_ENTRIES; ret->processEntry++) {
			ProcessClusterEntry* entry = (ProcessClusterEntry*)(processBuffer + ret->processEntry * sizeof(ProcessClusterEntry));
			if (entry->address == address) {
				// found it yay!
				ret->pageCluster = entry->pageCluster;
				return;
			}
			if (entry->address == 0) {
				// got to the end without finding the pid, create a new page cluster
				ret->pageCluster = getNextFreeCluster();
				char pageBuffer[ClusterSize];
				memset(pageBuffer, 0, ClusterSize);
				partition->writeCluster(ret->pageCluster, pageBuffer);

				entry->address = address;
				entry->pageCluster = ret->pageCluster;
				partition->writeCluster(ret->processCluster, processBuffer);

				return;
			}
		}
		prevProcessCluster = ret->processCluster;
		ret->processCluster = *((ClusterNo*)processBuffer);
	} while (ret->processCluster);

	// got to the end, haven't found it, and need to create a new process cluster first, then new page cluster *sigh*
	ClusterNo newProcessCluster = getNextFreeCluster();
	*((ClusterNo*)processBuffer) = newProcessCluster;
	partition->writeCluster(prevProcessCluster, processBuffer);

	ret->pageCluster = getNextFreeCluster();
	char pageBuffer[ClusterSize];
	memset(pageBuffer, 0, ClusterSize);
	partition->writeCluster(ret->pageCluster, pageBuffer);

	memset(processBuffer, 0, ClusterSize);
	ret->processEntry = 1;
	ProcessClusterEntry* entry = (ProcessClusterEntry*)(processBuffer + sizeof(ProcessClusterEntry));
	entry->address = address;
	entry->pageCluster = ret->pageCluster;
	partition->writeCluster(newProcessCluster, processBuffer);
}

void KernelSystem::writeToPartition(ProcessId pid, VirtualAddress startAddress, PageNum pageCount, void* content) {
	REPC repc;
	getProcessCluster(pid, &repc);
	for (PageNum currentPage = 0; currentPage < pageCount; currentPage++) {
		VirtualAddress currentAddress = startAddress + currentPage * PAGE_SIZE;
		PEPC pepc;
		getPageCluster(repc.processCluster, currentAddress, &pepc);
		char* currentContent = (char*)content + currentPage * PAGE_SIZE;
		partition->writeCluster(pepc.pageCluster, currentContent);
	}

	//printRootClusterTop();
	//printProcessClusterTop(pid);
	//printPageClusterTop(pid, startAddress);
}

void KernelSystem::writeToPartition_s(ProcessId pid, VirtualAddress startAddress, PageNum pageCount, void* content) {
	std::unique_lock<std::mutex> lock(_mutex);
	writeToPartition(pid, startAddress, pageCount, content);
}

void KernelSystem::erasePageFromPartition_s(ProcessId pid, VirtualAddress address) {
	std::unique_lock<std::mutex> lock(_mutex);
	REPC repc;
	getProcessCluster(pid, &repc);
	PEPC pepc;
	getPageCluster(repc.processCluster, address, &pepc);

	char buffer[ClusterSize];
	*((ClusterNo*)buffer) = freeClusterList;
	freeClusterList = pepc.pageCluster;
	partition->writeCluster(pepc.pageCluster, buffer);

	partition->readCluster(pepc.processCluster, buffer);
	ProcessClusterEntry* entry = (ProcessClusterEntry*)(buffer + pepc.processEntry * sizeof(ProcessClusterEntry));
	entry->address = -1;
	partition->writeCluster(pepc.processCluster, buffer);

	//printRootClusterTop();
	//printProcessClusterTop(pid);
	//printPageClusterTop(pid, address);
}

void KernelSystem::eraseProcessFromPartition_s(ProcessId pid) {
	std::unique_lock<std::mutex> lock(_mutex);
	REPC repc;
	getProcessCluster(pid, &repc);

	// iterate through and erase the page clusters
	ClusterNo processCluster = repc.processCluster;
	char processBuffer[ClusterSize];
	char pageBuffer[ClusterSize];
	do {
		partition->readCluster(processCluster, processBuffer);
		for (unsigned entryNum = 1; entryNum < PROCESS_CLUSTER_ENTRIES; entryNum++) {
			ProcessClusterEntry* pce = (ProcessClusterEntry*)(processBuffer + entryNum * sizeof(ProcessClusterEntry));
			if (!pce->address) {
				break;
			}
			if (pce->address == -1) {
				continue;
			}
			*((ClusterNo*)pageBuffer) = freeClusterList;
			freeClusterList = pce->pageCluster;
			partition->writeCluster(pce->pageCluster, pageBuffer);
		}
		ClusterNo nextProcessCluster = *((ClusterNo*)processBuffer);
		*((ClusterNo*)processBuffer) = freeClusterList;
		partition->writeCluster(processCluster, processBuffer);
		freeClusterList = processCluster;
		processCluster = nextProcessCluster;
	} while (processCluster);

	// erase the process entry from the root cluster
	partition->readCluster(repc.rootCluster, processBuffer);
	RootClusterEntry* rce = (RootClusterEntry*)(processBuffer + repc.rootEntry * sizeof(RootClusterEntry));
	rce->pid = -1;
	partition->writeCluster(repc.rootCluster, processBuffer);

	//printRootClusterTop();
	//printProcessClusterTop(pid);
}

void KernelSystem::loadFromPartition_s(ProcessId pid, VirtualAddress virtualAddress, PhysicalAddress physicalAddress) {
	std::unique_lock<std::mutex> lock(_mutex);
	REPC repc;
	getProcessCluster(pid, &repc);
	PEPC pepc;
	getPageCluster(repc.processCluster, virtualAddress, &pepc);
	partition->readCluster(pepc.pageCluster, (char*)physicalAddress);

	//printRootClusterTop();
	//printProcessClusterTop(pid);
	//printPageClusterTop(pid, virtualAddress);
}

PhysicalAddress KernelSystem::ejectPageAndGetFrame_s() {
	std::unique_lock<std::mutex> lock(_mutex);

	// get victim process
	PageNum totalVirtualMemory = getTotalVirtualMemory();
	PageNum totalPhysicalMemory = processVMSpaceSize;
	if (!processClockHand) {
		processClockHand = 1;
	}
	KernelProcess* victimProcess;
	PageNum processVirtualMemory, processPhysicalMemory;
	double physicalMemoryRatio, virtualMemoryRatio;
	for (unsigned i = 0; i < processMap.size(); i++) {
		// if the process has more of it's total virtual memory mapped to physical frames compared to the average,
		// force it to eject a page
		victimProcess = processMap[processClockHand]->pProcess;
		processClockHand = (processClockHand % processMap.size()) + 1;
		processVirtualMemory = victimProcess->getTotalVirtualMemory();
		processPhysicalMemory = victimProcess->getTotalPhysicalMemory();
		//PageNum processActualPhysicalMemory = victimProcess->getActualPhysicalMemory();

		//if (processPhysicalMemory != processActualPhysicalMemory) {
		//	printf("!!!\n");
		//}

		physicalMemoryRatio = (double)processPhysicalMemory / totalPhysicalMemory;
		virtualMemoryRatio = (double)processVirtualMemory / totalVirtualMemory;
		if (physicalMemoryRatio >= virtualMemoryRatio) {
			PhysicalAddress frame = victimProcess->ejectPageAndGetFrame_s();
			if (frame) {
				firstEjectHappened = true;
				//printf("Done find victim process and eject page\n");
				return frame;
			}
		}
	}
	// we've gone around full circle and not found anything, our math is bad :(
	//printf("All processes have been checked for victim pages, but none can be ejected\n");
	throw std::exception();
}

PageNum KernelSystem::getTotalVirtualMemory() {
	PageNum retVal = 0;
	for (auto p : processMap) {
		retVal += p.second->pProcess->getTotalVirtualMemory();
	}
	return retVal;
}

void KernelSystem::printFreeClustersTop() {
	char buffer[ClusterSize];
	printf("\n +========== FREE CLUSTERS TOP ==========\n");
	printf(" | ");
	ClusterNo currentCluster = freeClusterList;
	for (int i = 0; i < 5; i++) {
		partition->readCluster(currentCluster, buffer);
		printf("%06lu -> ", currentCluster);
		currentCluster = *((ClusterNo*)buffer);
		if (!currentCluster) {
			break;
		}
	}
	printf("\n +---------------------------------------\n");
}

void KernelSystem::printRootClusterTop() {
	char buffer[ClusterSize];
	partition->readCluster(0, buffer);
	printf("\n +========== ROOT CLUSTER TOP ==========\n");
	printf(" | %06lu", *((ClusterNo*)buffer));
	printf("\n +--------------------------------------\n");
	for (int i = 1; i < 5; i++) {
		RootClusterEntry* entry = (RootClusterEntry*)(buffer + i * sizeof(RootClusterEntry));
		printf(" | %04u | %06lu", entry->pid, entry->processCluster);
		printf("\n +--------------------------------------\n");
	}
}

void KernelSystem::printProcessClusterTop(ProcessId pid) {
	REPC repc;
	getProcessCluster(pid, &repc);
	char buffer[ClusterSize];
	partition->readCluster(repc.processCluster, buffer);
	printf("\n +========== PROCESS CLUSTER TOP ==========\n");
	printf(" | %06lu", *((ClusterNo*)buffer));
	printf("\n +-----------------------------------------\n");
	for (int i = 1; i < 5; i++) {
		ProcessClusterEntry* entry = (ProcessClusterEntry*)(buffer + i * sizeof(ProcessClusterEntry));
		printf(" | %06lx | %06lu", entry->address, entry->pageCluster);
		printf("\n +-----------------------------------------\n");
	}
}

void KernelSystem::printPageClusterTop(ProcessId pid, VirtualAddress address) {
	REPC repc;
	getProcessCluster(pid, &repc);
	PEPC pepc;
	getPageCluster(repc.processCluster, address, &pepc);
	char buffer[ClusterSize];
	partition->readCluster(pepc.pageCluster, buffer);
	printf("\n +========== PAGE CLUSTER TOP ==========\n");
	printf(" | ");
	for (int i = 0; i < 64; i++) {
		printf("%02x ", buffer[i]);
	}
	printf("\n +--------------------------------------\n");
}

void KernelSystem::giveToBuddySystem(PhysicalAddress startAddress, PageNum pageCount) {
	int currentBuddySystemLevel = 0;
	for (PageNum tempBuddySpaceSize = pageCount; tempBuddySpaceSize; tempBuddySpaceSize >>= 1) {
		if (tempBuddySpaceSize & 1) {
			buddySystem[currentBuddySystemLevel].insert(startAddress);
			startAddress = (PhysicalAddress)((uint64_t)startAddress + ((1 << currentBuddySystemLevel) * PAGE_SIZE));
		}
		currentBuddySystemLevel++;
	}
}

void KernelSystem::giveToBuddySystem_s(PhysicalAddress startAddress, PageNum pageCount) {
	std::unique_lock<std::mutex> lock(_mutex);
	giveToBuddySystem(startAddress, pageCount);
}

PhysicalAddress KernelSystem::takeFromBuddySystem_s(PageNum pageCount) {
	std::unique_lock<std::mutex> lock(_mutex);
	int currentLevel = 0;
	for (PageNum tempPageCount = pageCount - 1; tempPageCount; tempPageCount >>= 1) {
		currentLevel++;
	}
	for (; currentLevel < buddySystemLevelCount; currentLevel++) {
		BuddySystemLevel *theLevel = &(buddySystem[currentLevel]);
		if (!theLevel->empty()) {
			auto first = theLevel->begin();
			PhysicalAddress oldAddr = *first;
			theLevel->erase(first);
			PhysicalAddress newAddr = (PhysicalAddress)((uint64_t)oldAddr + (pageCount * PAGE_SIZE));
			PageNum extraSpaceToGiveBack = (1 << currentLevel) - pageCount;
			if (extraSpaceToGiveBack > 0) {
				giveToBuddySystem(newAddr, extraSpaceToGiveBack);
				defragmentBuddySystem();
			}
			return oldAddr;
		}
	}
	return 0;
}

void KernelSystem::defragmentBuddySystem() {
	//printBuddySystem();
	for (int currentLevel = 0; currentLevel < buddySystemLevelCount; currentLevel++) {
		BuddySystemLevel *theLevel = &(buddySystem[currentLevel]);
		auto previous = theLevel->end();
		for (auto current = theLevel->begin(); current != theLevel->end();) {
			PhysicalAddress previousAddr = (previous != theLevel->end()) ? *previous : (PhysicalAddress)(-1);
			PhysicalAddress currentAddr = *current;
			if ((previousAddr != (PhysicalAddress)(-1))
				&& (currentAddr == (PhysicalAddress)((uint64_t)previousAddr + (1 << currentLevel) * PAGE_SIZE))) {
				auto temp = current;
				current++;
				theLevel->erase(previous);
				theLevel->erase(temp);
				giveToBuddySystem(previousAddr, 2 << currentLevel);
				previous = theLevel->end();
			}
			else {
				previous = current;
				current++;
			}
		}
	}
	//printBuddySystem();
}

void KernelSystem::defragmentBuddySystem_s() {
	std::unique_lock<std::mutex> lock(_mutex);
	defragmentBuddySystem();
}

void KernelSystem::printBuddySystem() {
	printf("\n +========== BUDDY ==========\n");
	for (int currentLevel = 0; currentLevel < buddySystemLevelCount; currentLevel++) {
		BuddySystemLevel theLevel = buddySystem[currentLevel];
		if (!theLevel.empty()) {
			printf(" | %02d | ", currentLevel);
			for (PhysicalAddress addr : theLevel) {
				printf("%p, ", addr);
			}
			printf("\n +---------------------------\n");
		}
	}
}

void KernelSystem::giveToPmtPool(PhysicalAddress address) {
	pmtPool.insert(address);
}

void KernelSystem::giveToPmtPool_s(PhysicalAddress address) {
	std::unique_lock<std::mutex> lock(_mutex);
	giveToPmtPool(address);
}

PhysicalAddress KernelSystem::takeFromPmtPool_s() {
	std::unique_lock<std::mutex> lock(_mutex);
	if (pmtPool.empty()) {
		return 0;
	}
	auto first = pmtPool.begin();
	PhysicalAddress retVal = *first;
	pmtPool.erase(first);
	return retVal;
}

void KernelSystem::printPmtPoolTop() {
	printf("\n +========== PMT POOL TOP ==========\n");
	printf(" | ");
	int i = 0;
	for (PhysicalAddress current : pmtPool) {
		if (i++ >= 8) {
			break;
		}
		printf("%p, ", current);
	}
	printf("\n +----------------------------------\n");
}
