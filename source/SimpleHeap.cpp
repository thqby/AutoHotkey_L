﻿/*
AutoHotkey

Copyright 2003-2009 Chris Mallett (support@autohotkey.com)

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "stdafx.h" // pre-compiled headers
#include "SimpleHeap.h"
#include "globaldata.h" // for g_script, so that errors can be centrally reported here.

// Static member data:
// SimpleHeap *SimpleHeap::sFirst = NULL;
// SimpleHeap *SimpleHeap::sLast  = NULL;
// char *SimpleHeap::sMostRecentlyAllocated = NULL;
// UINT SimpleHeap::sBlockCount = 0;

LPTSTR SimpleHeap::strDup(LPCTSTR aBuf, size_t aLength)
// v1.0.44.14: Added aLength to improve performance in cases where callers already know the length.
// If aLength is at its default of -1, the length will be calculated here.
// Caller must ensure that aBuf isn't NULL.
{
	if (!aBuf || !*aBuf) // aBuf is checked for NULL because it's not worth avoiding it for such a low-level, frequently-called function.
		return _T(""); // Return the constant empty string to the caller (not aBuf itself since that might be volatile).
	if (aLength == -1) // Caller wanted us to calculate it.  Compare directly to -1 since aLength is unsigned.
		aLength = _tcslen(aBuf);
	LPTSTR new_buf;
	if (   !(new_buf = (LPTSTR)this->Malloc((aLength + 1) * sizeof(TCHAR)))   ) // +1 for the zero terminator.
		return NULL; // Callers may rely on NULL vs. "" being returned in the event of failure.
	if (aLength)
		tmemcpy(new_buf, aBuf, aLength); // memcpy() typically benchmarks slightly faster than strcpy().
	//else only a terminator is needed.
	new_buf[aLength] = '\0'; // Terminate here for when aLength==0 and for the memcpy above so that caller's aBuf doesn't have to be terminated.
	return new_buf;
}

LPTSTR SimpleHeap::Malloc(LPCTSTR aBuf, size_t aLength)
{
	auto new_buf = strDup(aBuf, aLength);
	if (!new_buf)
		MemoryError();
	return new_buf; // May be null.
}

LPTSTR SimpleHeap::Alloc(LPCTSTR aBuf, size_t aLength)
{
	auto new_buf = strDup(aBuf, aLength);
	if (!new_buf)
		CriticalFail(); // This terminates the program.
	return new_buf; // Always non-null.
}

void* SimpleHeap::Malloc(size_t aSize)
// This could be made more memory efficient by searching old blocks for sufficient
// free space to handle <size> prior to creating a new block.  But the whole point
// of this class is that it's only called to allocate relatively small objects,
// such as the lines of text in a script file.  The length of such lines is typically
// around 80, and only rarely would exceed 1000.  Trying to find memory in old blocks
// seems like a bad trade-off compared to the performance impact of traversing a
// potentially large linked list or maintaining and traversing an array of
// "under-utilized" blocks.
{
	if (aSize < 1)
		return NULL;
	// Use one block only for initialization so first block will have mPrevBlock
	if (!sFirst) // We need at least one block to do anything, so create it.
		if (   !(sFirst = CreateBlock(1)) || !(sFirst->mNextBlock = CreateBlock(BLOCK_SIZE))   )
			return NULL;
		else
		{
			sLast = sFirst->mNextBlock;  // Constructing a new block always results in it becoming the current block.
			sLast->mPrevBlock = sFirst;
		}
	if (aSize > sLast->mSpaceAvailable)
	{
		if (aSize > MAX_ALLOC_IN_NEW_BLOCK) // Also covers aSize > BLOCK_SIZE.
		{
			// insert a newly allocated block of required size before last block to avoid wasting the remainder of the block.
			SimpleHeap *aPrevBlock = sLast->mPrevBlock, *aLastBlock = sLast, *newblock;
			if (!(aPrevBlock->mNextBlock = newblock = CreateBlock(aSize)))
				return NULL;
			sLast = aLastBlock;
			sLast->mPrevBlock = newblock;
			newblock->mPrevBlock = aPrevBlock;
			newblock->mNextBlock = sLast;
			newblock->mFreeMarker += aSize;
			newblock->mSpaceAvailable = 0;
			return newblock->mBlock;
		}
		if (!(sLast->mNextBlock = CreateBlock(BLOCK_SIZE)))
			return NULL;
		else
			sLast = sLast->mNextBlock;
	}
	sMostRecentlyAllocated = sLast->mFreeMarker; // THIS IS NOW THE NEWLY ALLOCATED BLOCK FOR THE CALLER, which is 32-bit aligned because the previous call to this function (i.e. the logic below) set it up that way.
	// v1.0.40.04: Set up the NEXT chunk to be aligned on a 32-bit boundary (the first chunk in each block
	// should always be aligned since the block's address came from malloc()).  On average, this change
	// "wastes" only 1.5 bytes per chunk. In a 200 KB script of typical contents, this change requires less
	// than 8 KB of additional memory (as shown by temporarily making BLOCK_SIZE a smaller value such as 8 KB
	// for a more accurate measurement).  That cost seems well worth the following benefits:
	// 1) Solves failure of API functions like GetRawInputDeviceList() when passed a non-aligned address.
	// 2) May solve other obscure issues (past and future), which improves sanity due to not chasing bugs
	//    for hours on end that were caused solely by non-alignment.
	// 3) May slightly improve performance since aligned data is easier for the CPU to access and cache.
	size_t remainder = aSize % sizeof(void *);
	size_t size_consumed = remainder ? aSize + (sizeof(void *) - remainder) : aSize;
	// v1.0.45: The following can't happen when BLOCK_SIZE is a multiple of 4, so it's commented out:
	//if (size_consumed > sLast->mSpaceAvailable) // For maintainability, don't allow mFreeMarker to go out of bounds or
	//	size_consumed = sLast->mSpaceAvailable; // mSpaceAvailable to go negative (which it can't due to be unsigned).
	sLast->mFreeMarker += size_consumed;
	sLast->mSpaceAvailable -= size_consumed;
	return (void *)sMostRecentlyAllocated;
}

void* SimpleHeap::Alloc(size_t aSize)
{
	auto p = Malloc(aSize);
	if (!p)
		CriticalFail();
	return p;
}



void SimpleHeap::Delete(void *aPtr)
// If aPtr is the most recently allocated area of memory by SimpleHeap, this will reclaim that
// memory.  Otherwise, the caller should realize that the memory cannot be reclaimed (i.e. potential
// memory leak unless caller handles things right).
{
	if (aPtr != sMostRecentlyAllocated || !sMostRecentlyAllocated)
		return;
	size_t sMostRecentlyAllocated_size = sLast->mFreeMarker - sMostRecentlyAllocated;
	sLast->mFreeMarker -= sMostRecentlyAllocated_size;
	sLast->mSpaceAvailable += sMostRecentlyAllocated_size;
	sMostRecentlyAllocated = NULL; // i.e. no support for anything other than a one-time delete of an item just added.
}






SimpleHeap *SimpleHeap::CreateBlock(size_t aSize)
// Added for v1.0.40.04 to try to solve the fact that some functions such as GetRawInputDeviceList()
// will sometimes fail if passed memory from SimpleHeap. Although this change didn't actually solve
// the issue (it turned out to be a 32-bit alignment issue), using malloc() appears to save memory
// (compared to using "new" on a class that contains a large buffer such as "char mBlock[BLOCK_SIZE]").
// In a 200 KB script, it saves 8 KB of VM Size as shown by Task Manager.
{
	SimpleHeap *block = new SimpleHeap;
	// The new block's mFreeMarker starts off pointing to the first byte in the new block:
	if (   !(block->mBlock = block->mFreeMarker = (char *)malloc(aSize))   )
	{
		delete block;
		return NULL;
	}
	// Since above didn't return, block was successfully created:
	block->mSpaceAvailable = aSize;
	block->mPrevBlock = sLast;
	++sBlockCount;
	return block;
}



SimpleHeap::SimpleHeap()  // Construct a new block.  Caller is responsible for initializing other members.
	: sFirst(NULL), sLast(NULL), mFreeMarker(NULL)
	, mSpaceAvailable(0), sMostRecentlyAllocated(NULL)
	, mNextBlock(NULL), mBlock(NULL), mPrevBlock(NULL), sBlockCount(0)
{
}


SimpleHeap::~SimpleHeap()
// This destructor is currently never called because all instances of the object are created
// with "new", yet none are ever destroyed with "delete".  As an alternative to this behavior
// the delete method should recursively delete mNextBlock, if it's non-NULL, prior to
// returning.  It seems unnecessary to do this, however, since the whole idea behind this
// class is that it's a simple implementation of one-time, persistent memory allocation.
// It's not intended to permit deallocation and subsequent reclamation of freed fragments
// within the collection of blocks.  When the program exits, all memory dynamically
// allocated by the constructor and any other methods that call "new" will be reclaimed
// by the OS.  UPDATE: This is now called by static method DeleteAll().
{
	SimpleHeap *next, *curr;
	for (curr = sFirst; curr;)
	{
		next = curr->mNextBlock;  // Save this member's value prior to deleting the object.
		delete curr;
		curr = next;
	}
	free(mBlock);
	return;
}



void SimpleHeap::Merge(SimpleHeap* aHeap)
{
	if (aHeap->sBlockCount) {
		if (sBlockCount) {
			sLast->mNextBlock = aHeap->sFirst->mNextBlock;
			aHeap->sFirst->mNextBlock->mPrevBlock = sLast;
			sBlockCount += aHeap->sBlockCount - 1;
			aHeap->sFirst->mNextBlock = nullptr;
		}
		else {
			mBlock = aHeap->mBlock;
			sBlockCount = aHeap->sBlockCount;
			sFirst = aHeap->sFirst;
			mNextBlock = aHeap->mNextBlock, mPrevBlock = aHeap->mPrevBlock;
			aHeap->sFirst = nullptr;
		}
		sLast = aHeap->sLast;
		mFreeMarker = aHeap->mFreeMarker;
		mSpaceAvailable = aHeap->mSpaceAvailable;
		sMostRecentlyAllocated = aHeap->sMostRecentlyAllocated;
	}
	delete aHeap;
}
