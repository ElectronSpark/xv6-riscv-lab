#!/usr/bin/env python3
"""
Page Allocation Log Analyzer

This script analyzes page allocation logs from xv6-riscv kernel.
It tracks page_alloc and page_free events and provides statistics.

Usage: python3 page_log_analyzer.py <log_file>
"""

import re
import sys
from collections import defaultdict, Counter
from dataclasses import dataclass
from typing import Dict, List, Optional, Set


@dataclass
class PageEvent:
    """Represents a page allocation or deallocation event"""
    event_type: str  # 'alloc' or 'free'
    order: int
    flags: int
    page_addr: int
    line_number: int

@dataclass
class SlabEvent:
    """Represents a slab allocation or deallocation event"""
    event_type: str  # 'alloc' or 'free'
    cache_name: str
    cache_ptr: int
    obj_addr: int
    obj_size: int
    line_number: int

@dataclass
class DoubleAllocationError:
    """Represents a double allocation error"""
    page_addr: int
    first_alloc_line: int
    first_alloc_order: int
    first_alloc_flags: int
    second_alloc_line: int
    second_alloc_order: int
    second_alloc_flags: int

@dataclass
class DoubleFreeError:
    """Represents a double free or free-without-allocation error"""
    page_addr: int
    free_line: int
    free_order: int
    free_flags: int
    error_type: str  # 'double_free' or 'free_without_alloc'
    original_alloc_line: Optional[int] = None

@dataclass
class SlabDoubleAllocationError:
    """Represents a slab double allocation error"""
    obj_addr: int
    cache_name: str
    first_alloc_line: int
    first_alloc_cache: str
    first_alloc_size: int
    second_alloc_line: int
    second_alloc_cache: str
    second_alloc_size: int

@dataclass
class SlabDoubleFreeError:
    """Represents a slab double free or free-without-allocation error"""
    obj_addr: int
    cache_name: str
    free_line: int
    free_size: int
    error_type: str  # 'double_free' or 'free_without_alloc'
    original_alloc_line: Optional[int] = None

@dataclass
class BuddyInitEvent:
    """Represents buddy system initialization"""
    start_addr: int
    end_addr: int
    line_number: int

@dataclass
class SlabPageValidationError:
    """Represents a slab object not in a SLAB-flagged page"""
    obj_addr: int
    cache_name: str
    obj_size: int
    line_number: int
    page_addr: int
    page_flags: Optional[int] = None
    error_type: str = 'slab_not_in_slab_page'  # 'slab_not_in_slab_page' or 'page_not_allocated'

@dataclass
class PageRangeValidationError:
    """Represents a page allocation outside buddy system range"""
    page_addr: int
    order: int
    flags: int
    line_number: int
    buddy_start: int
    buddy_end: int


class PageAllocAnalyzer:
    """Analyzes page allocation logs"""
    
    def __init__(self):
        self.events: List[PageEvent] = []
        self.slab_events: List[SlabEvent] = []
        self.buddy_init_event: Optional[BuddyInitEvent] = None
        self.allocated_pages: Dict[int, PageEvent] = {}  # page_addr -> alloc_event
        self.freed_pages: Dict[int, PageEvent] = {}  # page_addr -> last_free_event
        self.allocated_slab_objs: Dict[int, SlabEvent] = {}  # obj_addr -> alloc_event
        self.freed_slab_objs: Dict[int, SlabEvent] = {}  # obj_addr -> last_free_event
        
        self.allocation_stats = Counter()
        self.deallocation_stats = Counter()
        self.flag_stats = Counter()
        self.order_stats = Counter()
        
        # Slab statistics
        self.slab_allocation_stats = Counter()  # by cache name
        self.slab_deallocation_stats = Counter()  # by cache name
        self.slab_size_stats = Counter()  # by object size
        
        # Error tracking
        self.double_allocations: List[DoubleAllocationError] = []
        self.double_frees: List[DoubleFreeError] = []
        self.slab_double_allocations: List[SlabDoubleAllocationError] = []
        self.slab_double_frees: List[SlabDoubleFreeError] = []
        self.slab_page_validation_errors: List[SlabPageValidationError] = []
        self.page_range_validation_errors: List[PageRangeValidationError] = []
        
    def parse_log_file(self, filename: str) -> None:
        """Parse the log file and extract page allocation events"""
        # Regex patterns for page allocation and deallocation
        alloc_pattern = r'page_alloc: order (\d+), flags (0x[0-9a-fA-F]+), page (0x[0-9a-fA-F]+)'
        free_pattern = r'page_free: order (\d+), flags (0x[0-9a-fA-F]+), page (0x[0-9a-fA-F]+)'
        
        # Regex patterns for slab allocation and deallocation
        slab_alloc_pattern = r'slab_alloc: cache ([^(]+)\(([^)]+)\), obj (0x[0-9a-fA-F]+), size: (\d+)'
        slab_free_pattern = r'slab_free: cache ([^(]+)\(([^)]+)\), obj (0x[0-9a-fA-F]+), size: (\d+)'
        
        # Regex pattern for buddy system initialization
        buddy_init_pattern = r'page_buddy_init\(\): buddy pages from (0x[0-9a-fA-F]+) to (0x[0-9a-fA-F]+)'
        
        try:
            with open(filename, 'r') as f:
                for line_num, line in enumerate(f, 1):
                    line = line.strip()
                    
                    # Check for buddy system initialization
                    buddy_init_match = re.search(buddy_init_pattern, line)
                    if buddy_init_match:
                        start_addr = int(buddy_init_match.group(1), 16)
                        end_addr = int(buddy_init_match.group(2), 16)
                        self.buddy_init_event = BuddyInitEvent(start_addr, end_addr, line_num)
                        print(f"Found buddy system range: 0x{start_addr:x} to 0x{end_addr:x}")
                        continue
                    
                    # Check for page allocation event
                    alloc_match = re.search(alloc_pattern, line)
                    if alloc_match:
                        order = int(alloc_match.group(1))
                        flags = int(alloc_match.group(2), 16)
                        page_addr = int(alloc_match.group(3), 16)
                        
                        event = PageEvent('alloc', order, flags, page_addr, line_num)
                        self.events.append(event)
                        self._process_allocation(event)
                        continue
                    
                    # Check for page deallocation event
                    free_match = re.search(free_pattern, line)
                    if free_match:
                        order = int(free_match.group(1))
                        flags = int(free_match.group(2), 16)
                        page_addr = int(free_match.group(3), 16)
                        
                        event = PageEvent('free', order, flags, page_addr, line_num)
                        self.events.append(event)
                        self._process_deallocation(event)
                        continue
                    
                    # Check for slab allocation event
                    slab_alloc_match = re.search(slab_alloc_pattern, line)
                    if slab_alloc_match:
                        cache_name = slab_alloc_match.group(1)
                        cache_ptr = int(slab_alloc_match.group(2), 16)
                        obj_addr = int(slab_alloc_match.group(3), 16)
                        obj_size = int(slab_alloc_match.group(4))
                        
                        event = SlabEvent('alloc', cache_name, cache_ptr, obj_addr, obj_size, line_num)
                        self.slab_events.append(event)
                        self._process_slab_allocation(event)
                        continue
                    
                    # Check for slab deallocation event
                    slab_free_match = re.search(slab_free_pattern, line)
                    if slab_free_match:
                        cache_name = slab_free_match.group(1)
                        cache_ptr = int(slab_free_match.group(2), 16)
                        obj_addr = int(slab_free_match.group(3), 16)
                        obj_size = int(slab_free_match.group(4))
                        
                        event = SlabEvent('free', cache_name, cache_ptr, obj_addr, obj_size, line_num)
                        self.slab_events.append(event)
                        self._process_slab_deallocation(event)
                        
        except FileNotFoundError:
            print(f"Error: File '{filename}' not found")
            sys.exit(1)
        except Exception as e:
            print(f"Error reading file: {e}")
            sys.exit(1)
    
    def _process_allocation(self, event: PageEvent) -> None:
        """Process a page allocation event"""
        # Validate page range first
        self._validate_page_range(event)
        
        # Calculate number of pages allocated (2^order)
        pages_count = 1 << event.order
        
        # Record allocation for each page in the range
        for i in range(pages_count):
            page_addr = event.page_addr + (i * 4096)  # Assuming 4KB pages
            if page_addr in self.allocated_pages:
                # Double allocation detected
                prev_alloc = self.allocated_pages[page_addr]
                double_alloc = DoubleAllocationError(
                    page_addr=page_addr,
                    first_alloc_line=prev_alloc.line_number,
                    first_alloc_order=prev_alloc.order,
                    first_alloc_flags=prev_alloc.flags,
                    second_alloc_line=event.line_number,
                    second_alloc_order=event.order,
                    second_alloc_flags=event.flags
                )
                self.double_allocations.append(double_alloc)
                print(f"ERROR: Double allocation detected at 0x{page_addr:x}")
                print(f"  First allocation: Line {prev_alloc.line_number} (order {prev_alloc.order}, flags 0x{prev_alloc.flags:x})")
                print(f"  Second allocation: Line {event.line_number} (order {event.order}, flags 0x{event.flags:x})")
            
            self.allocated_pages[page_addr] = event
        
        # Update statistics
        self.allocation_stats[event.order] += 1
        self.flag_stats[event.flags] += 1
        self.order_stats[event.order] += pages_count
    
    def _process_deallocation(self, event: PageEvent) -> None:
        """Process a page deallocation event"""
        # Calculate number of pages freed (2^order)
        pages_count = 1 << event.order
        
        # Remove allocation records for each page in the range
        for i in range(pages_count):
            page_addr = event.page_addr + (i * 4096)  # Assuming 4KB pages
            
            if page_addr not in self.allocated_pages:
                # Check if this is a double free or free without allocation
                if page_addr in self.freed_pages:
                    # Double free
                    prev_free = self.freed_pages[page_addr]
                    double_free = DoubleFreeError(
                        page_addr=page_addr,
                        free_line=event.line_number,
                        free_order=event.order,
                        free_flags=event.flags,
                        error_type='double_free',
                        original_alloc_line=prev_free.line_number
                    )
                    self.double_frees.append(double_free)
                    print(f"ERROR: Double free detected at 0x{page_addr:x}")
                    print(f"  Previous free: Line {prev_free.line_number} (order {prev_free.order}, flags 0x{prev_free.flags:x})")
                    print(f"  Current free: Line {event.line_number} (order {event.order}, flags 0x{event.flags:x})")
                else:
                    # Free without allocation
                    double_free = DoubleFreeError(
                        page_addr=page_addr,
                        free_line=event.line_number,
                        free_order=event.order,
                        free_flags=event.flags,
                        error_type='free_without_alloc'
                    )
                    self.double_frees.append(double_free)
                    print(f"ERROR: Free without allocation at 0x{page_addr:x} (line {event.line_number})")
            else:
                # Normal free - remove from allocated pages
                del self.allocated_pages[page_addr]
            
            # Record this free event
            self.freed_pages[page_addr] = event
        
        # Update statistics
        self.deallocation_stats[event.order] += 1
    
    def _process_slab_allocation(self, event: SlabEvent) -> None:
        """Process a slab allocation event"""
        # Validate that slab object is in a SLAB-flagged page
        self._validate_slab_in_slab_page(event)
        
        if event.obj_addr in self.allocated_slab_objs:
            # Double slab allocation detected
            prev_alloc = self.allocated_slab_objs[event.obj_addr]
            double_alloc = SlabDoubleAllocationError(
                obj_addr=event.obj_addr,
                cache_name=event.cache_name,
                first_alloc_line=prev_alloc.line_number,
                first_alloc_cache=prev_alloc.cache_name,
                first_alloc_size=prev_alloc.obj_size,
                second_alloc_line=event.line_number,
                second_alloc_cache=event.cache_name,
                second_alloc_size=event.obj_size
            )
            self.slab_double_allocations.append(double_alloc)
            print(f"ERROR: SLAB double allocation detected at 0x{event.obj_addr:x}")
            print(f"  First allocation: Line {prev_alloc.line_number} (cache {prev_alloc.cache_name}, size {prev_alloc.obj_size})")
            print(f"  Second allocation: Line {event.line_number} (cache {event.cache_name}, size {event.obj_size})")
        
        # Record this allocation
        self.allocated_slab_objs[event.obj_addr] = event
        
        # Update statistics
        self.slab_allocation_stats[event.cache_name] += 1
        self.slab_size_stats[event.obj_size] += 1
    
    def _process_slab_deallocation(self, event: SlabEvent) -> None:
        """Process a slab deallocation event"""
        if event.obj_addr not in self.allocated_slab_objs:
            # Check if this is a double free or free without allocation
            if event.obj_addr in self.freed_slab_objs:
                # Double free
                prev_free = self.freed_slab_objs[event.obj_addr]
                double_free = SlabDoubleFreeError(
                    obj_addr=event.obj_addr,
                    cache_name=event.cache_name,
                    free_line=event.line_number,
                    free_size=event.obj_size,
                    error_type='double_free',
                    original_alloc_line=prev_free.line_number
                )
                self.slab_double_frees.append(double_free)
                print(f"ERROR: SLAB double free detected at 0x{event.obj_addr:x}")
                print(f"  Previous free: Line {prev_free.line_number} (cache {prev_free.cache_name}, size {prev_free.obj_size})")
                print(f"  Current free: Line {event.line_number} (cache {event.cache_name}, size {event.obj_size})")
            else:
                # Free without allocation
                double_free = SlabDoubleFreeError(
                    obj_addr=event.obj_addr,
                    cache_name=event.cache_name,
                    free_line=event.line_number,
                    free_size=event.obj_size,
                    error_type='free_without_alloc'
                )
                self.slab_double_frees.append(double_free)
                print(f"ERROR: SLAB free without allocation at 0x{event.obj_addr:x} (line {event.line_number})")
        else:
            # Normal free - remove from allocated objects
            del self.allocated_slab_objs[event.obj_addr]
        
        # Record this free event
        self.freed_slab_objs[event.obj_addr] = event
        
        # Update statistics
        self.slab_deallocation_stats[event.cache_name] += 1
    
    def _validate_page_range(self, event: PageEvent) -> None:
        """Validate that page allocation is within buddy system range"""
        if self.buddy_init_event is None:
            # Can't validate without buddy system range info
            return
        
        page_count = 1 << event.order
        page_size = 4096  # Assuming 4KB pages
        start_addr = event.page_addr
        end_addr = event.page_addr + (page_count * page_size)
        
        buddy_start = self.buddy_init_event.start_addr
        buddy_end = self.buddy_init_event.end_addr
        
        if start_addr < buddy_start or end_addr > buddy_end:
            error = PageRangeValidationError(
                page_addr=event.page_addr,
                order=event.order,
                flags=event.flags,
                line_number=event.line_number,
                buddy_start=buddy_start,
                buddy_end=buddy_end
            )
            self.page_range_validation_errors.append(error)
            print(f"ERROR: Page allocation outside buddy range at 0x{event.page_addr:x}")
            print(f"  Allocated range: 0x{start_addr:x} - 0x{end_addr:x}")
            print(f"  Buddy range: 0x{buddy_start:x} - 0x{buddy_end:x}")
    
    def _validate_slab_in_slab_page(self, event: SlabEvent) -> None:
        """Validate that slab object is in a page allocated with PAGE_FLAG_SLAB"""
        PAGE_FLAG_SLAB = 1 << 7  # From page_type.h
        page_size = 4096
        
        # Find the page containing this object
        page_addr = (event.obj_addr // page_size) * page_size
        
        # Check if there's a page allocated at this address with SLAB flag
        page_found = False
        page_has_slab_flag = False
        page_flags = None
        
        for allocated_page_addr, page_event in self.allocated_pages.items():
            page_count = 1 << page_event.order
            page_end = allocated_page_addr + (page_count * page_size)
            
            if allocated_page_addr <= event.obj_addr < page_end:
                page_found = True
                page_flags = page_event.flags
                if page_event.flags & PAGE_FLAG_SLAB:
                    page_has_slab_flag = True
                break
        
        if not page_found:
            # Slab object in a page that's not allocated
            error = SlabPageValidationError(
                obj_addr=event.obj_addr,
                cache_name=event.cache_name,
                obj_size=event.obj_size,
                line_number=event.line_number,
                page_addr=page_addr,
                error_type='page_not_allocated'
            )
            self.slab_page_validation_errors.append(error)
            print(f"ERROR: Slab object 0x{event.obj_addr:x} in unallocated page 0x{page_addr:x}")
            print(f"  Cache: {event.cache_name}, Size: {event.obj_size}, Line: {event.line_number}")
        elif not page_has_slab_flag:
            # Slab object in a page without SLAB flag
            error = SlabPageValidationError(
                obj_addr=event.obj_addr,
                cache_name=event.cache_name,
                obj_size=event.obj_size,
                line_number=event.line_number,
                page_addr=page_addr,
                page_flags=page_flags,
                error_type='slab_not_in_slab_page'
            )
            self.slab_page_validation_errors.append(error)
            print(f"ERROR: Slab object 0x{event.obj_addr:x} in page without SLAB flag")
            print(f"  Page: 0x{page_addr:x}, Flags: 0x{page_flags:x}, Cache: {event.cache_name}")
            print(f"  Expected PAGE_FLAG_SLAB (0x{PAGE_FLAG_SLAB:x}) but got 0x{page_flags:x}")
    
    def print_summary(self) -> None:
        """Print comprehensive analysis summary"""
        print("=" * 60)
        print("PAGE & SLAB ALLOCATION LOG ANALYSIS")
        print("=" * 60)
        
        total_alloc_events = sum(self.allocation_stats.values())
        total_free_events = sum(self.deallocation_stats.values())
        total_pages_allocated = sum(self.order_stats.values())
        currently_allocated = len(self.allocated_pages)
        
        total_slab_alloc_events = sum(self.slab_allocation_stats.values())
        total_slab_free_events = sum(self.slab_deallocation_stats.values())
        currently_allocated_slab_objs = len(self.allocated_slab_objs)
        
        print(f"\nOVERALL STATISTICS:")
        print(f"  PAGE ALLOCATIONS:")
        print(f"    Total allocation events: {total_alloc_events}")
        print(f"    Total deallocation events: {total_free_events}")
        print(f"    Total pages allocated: {total_pages_allocated}")
        print(f"    Currently allocated pages: {currently_allocated}")
        print(f"    Net allocations: {total_alloc_events - total_free_events}")
        
        print(f"  SLAB ALLOCATIONS:")
        print(f"    Total allocation events: {total_slab_alloc_events}")
        print(f"    Total deallocation events: {total_slab_free_events}")
        print(f"    Currently allocated objects: {currently_allocated_slab_objs}")
        print(f"    Net allocations: {total_slab_alloc_events - total_slab_free_events}")
        
        # Print error summary prominently
        total_page_errors = len(self.double_allocations) + len(self.double_frees) + len(self.page_range_validation_errors)
        total_slab_errors = len(self.slab_double_allocations) + len(self.slab_double_frees) + len(self.slab_page_validation_errors)
        
        print(f"\nERROR SUMMARY:")
        print(f"  PAGE ERRORS:")
        print(f"    Double allocations: {len(self.double_allocations)}")
        print(f"    Double frees: {len([df for df in self.double_frees if df.error_type == 'double_free'])}")
        print(f"    Frees without allocation: {len([df for df in self.double_frees if df.error_type == 'free_without_alloc'])}")
        print(f"    Range violations: {len(self.page_range_validation_errors)}")
        print(f"    Total page errors: {total_page_errors}")
        
        print(f"  SLAB ERRORS:")
        print(f"    Double allocations: {len(self.slab_double_allocations)}")
        print(f"    Double frees: {len([df for df in self.slab_double_frees if df.error_type == 'double_free'])}")
        print(f"    Frees without allocation: {len([df for df in self.slab_double_frees if df.error_type == 'free_without_alloc'])}")
        print(f"    Slab validation errors: {len(self.slab_page_validation_errors)}")
        print(f"    Total slab errors: {total_slab_errors}")
        
        total_errors = total_page_errors + total_slab_errors
        if total_errors > 0:
            print(f"  ⚠️  MEMORY MANAGEMENT ERRORS DETECTED! (Total: {total_errors})")
        else:
            print(f"  ✅ No double allocation or double free errors detected!")
        
        self._print_detailed_errors()
        
        print(f"\nPAGE ALLOCATION BY ORDER:")
        for order in sorted(self.allocation_stats.keys()):
            alloc_count = self.allocation_stats[order]
            free_count = self.deallocation_stats.get(order, 0)
            pages_per_alloc = 1 << order
            total_pages = alloc_count * pages_per_alloc
            print(f"  Order {order} (2^{order} = {pages_per_alloc} pages): "
                  f"{alloc_count} allocs, {free_count} frees, "
                  f"{total_pages} total pages")
        
        print(f"\nSLAB ALLOCATION BY CACHE:")
        for cache_name in sorted(self.slab_allocation_stats.keys()):
            alloc_count = self.slab_allocation_stats[cache_name]
            free_count = self.slab_deallocation_stats.get(cache_name, 0)
            print(f"  {cache_name}: {alloc_count} allocs, {free_count} frees, "
                  f"net: {alloc_count - free_count}")
        
        print(f"\nSLAB ALLOCATION BY OBJECT SIZE:")
        for obj_size in sorted(self.slab_size_stats.keys()):
            count = self.slab_size_stats[obj_size]
            print(f"  {obj_size} bytes: {count} allocations")
        
        print(f"\nPAGE FLAG USAGE:")
        flag_names = {
            0x0: "NONE",
            0x1: "SLAB", 
            0x2: "ANON",
            0x4: "PGTABLE",
            0x8: "LOCKED",
            0x10: "BUDDY"
        }
        
        for flags in sorted(self.flag_stats.keys()):
            count = self.flag_stats[flags]
            flag_str = flag_names.get(flags, f"UNKNOWN(0x{flags:x})")
            print(f"  {flag_str} (0x{flags:x}): {count} allocations")
        
        if currently_allocated > 0:
            print(f"\nCURRENTLY ALLOCATED PAGES:")
            allocated_by_order = defaultdict(int)
            allocated_by_flags = defaultdict(int)
            
            for page_addr, alloc_event in self.allocated_pages.items():
                allocated_by_order[alloc_event.order] += 1
                allocated_by_flags[alloc_event.flags] += 1
            
            print(f"  By order:")
            for order in sorted(allocated_by_order.keys()):
                pages_per_alloc = 1 << order
                print(f"    Order {order}: {allocated_by_order[order]} allocations "
                      f"({allocated_by_order[order] * pages_per_alloc} pages)")
            
            print(f"  By flags:")
            for flags in sorted(allocated_by_flags.keys()):
                flag_str = flag_names.get(flags, f"UNKNOWN(0x{flags:x})")
                print(f"    {flag_str}: {allocated_by_flags[flags]} allocations")
        
        if currently_allocated_slab_objs > 0:
            print(f"\nCURRENTLY ALLOCATED SLAB OBJECTS:")
            allocated_by_cache = defaultdict(int)
            allocated_by_size = defaultdict(int)
            
            for obj_addr, alloc_event in self.allocated_slab_objs.items():
                allocated_by_cache[alloc_event.cache_name] += 1
                allocated_by_size[alloc_event.obj_size] += 1
            
            print(f"  By cache:")
            for cache_name in sorted(allocated_by_cache.keys()):
                print(f"    {cache_name}: {allocated_by_cache[cache_name]} objects")
            
            print(f"  By size:")
            for obj_size in sorted(allocated_by_size.keys()):
                print(f"    {obj_size} bytes: {allocated_by_size[obj_size]} objects")
    
    def find_leaks(self) -> List[PageEvent]:
        """Find potential memory leaks (allocated but not freed pages)"""
        return [event for event in self.allocated_pages.values()]
    
    def print_leaks(self) -> None:
        """Print information about potential memory leaks"""
        leaks = self.find_leaks()
        if leaks:
            print(f"\nPOTENTIAL MEMORY LEAKS ({len(leaks)} pages):")
            for event in sorted(leaks, key=lambda x: x.line_number):
                pages_count = 1 << event.order
                print(f"  Line {event.line_number}: 0x{event.page_addr:x} "
                      f"(order {event.order}, {pages_count} pages, flags 0x{event.flags:x})")
        else:
            print(f"\nNo memory leaks detected!")
    
    def save_detailed_report(self, output_file: str) -> None:
        """Save detailed report to file"""
        with open(output_file, 'w') as f:
            f.write("PAGE ALLOCATION DETAILED REPORT\n")
            f.write("=" * 60 + "\n\n")
            
            f.write("ALL EVENTS:\n")
            for i, event in enumerate(self.events):
                pages_count = 1 << event.order
                f.write(f"{i+1:4d}. Line {event.line_number:4d}: "
                       f"{event.event_type:5s} 0x{event.page_addr:08x} "
                       f"(order {event.order}, {pages_count} pages, "
                       f"flags 0x{event.flags:x})\n")
            
            if self.allocated_pages:
                f.write(f"\nCURRENTLY ALLOCATED PAGES:\n")
                for page_addr in sorted(self.allocated_pages.keys()):
                    event = self.allocated_pages[page_addr]
                    f.write(f"  0x{page_addr:08x} - allocated at line {event.line_number} "
                           f"(order {event.order}, flags 0x{event.flags:x})\n")


    def _print_detailed_errors(self) -> None:
        """Print detailed information about double allocations and double frees"""
        if len(self.double_allocations) > 0:
            print(f"\nPAGE DOUBLE ALLOCATION ERRORS ({len(self.double_allocations)}):")
            print("-" * 50)
            for i, error in enumerate(self.double_allocations, 1):
                print(f"  {i}. Page 0x{error.page_addr:x}")
                print(f"     First allocation:  Line {error.first_alloc_line:4d} "
                      f"(order {error.first_alloc_order}, flags 0x{error.first_alloc_flags:x})")
                print(f"     Second allocation: Line {error.second_alloc_line:4d} "
                      f"(order {error.second_alloc_order}, flags 0x{error.second_alloc_flags:x})")
                print()
        
        if len(self.slab_double_allocations) > 0:
            print(f"\nSLAB DOUBLE ALLOCATION ERRORS ({len(self.slab_double_allocations)}):")
            print("-" * 50)
            for i, error in enumerate(self.slab_double_allocations, 1):
                print(f"  {i}. Object 0x{error.obj_addr:x}")
                print(f"     First allocation:  Line {error.first_alloc_line:4d} "
                      f"(cache {error.first_alloc_cache}, size {error.first_alloc_size})")
                print(f"     Second allocation: Line {error.second_alloc_line:4d} "
                      f"(cache {error.second_alloc_cache}, size {error.second_alloc_size})")
                print()
        
        if len(self.double_frees) > 0:
            double_free_errors = [df for df in self.double_frees if df.error_type == 'double_free']
            free_without_alloc_errors = [df for df in self.double_frees if df.error_type == 'free_without_alloc']
            
            if double_free_errors:
                print(f"\nPAGE DOUBLE FREE ERRORS ({len(double_free_errors)}):")
                print("-" * 50)
                for i, error in enumerate(double_free_errors, 1):
                    print(f"  {i}. Page 0x{error.page_addr:x}")
                    print(f"     Current free: Line {error.free_line:4d} "
                          f"(order {error.free_order}, flags 0x{error.free_flags:x})")
                    if error.original_alloc_line:
                        print(f"     Previous free: Line {error.original_alloc_line:4d}")
                    print()
            
            if free_without_alloc_errors:
                print(f"\nPAGE FREE WITHOUT ALLOCATION ERRORS ({len(free_without_alloc_errors)}):")
                print("-" * 50)
                for i, error in enumerate(free_without_alloc_errors, 1):
                    print(f"  {i}. Page 0x{error.page_addr:x}")
                    print(f"     Free attempt: Line {error.free_line:4d} "
                          f"(order {error.free_order}, flags 0x{error.free_flags:x})")
                    print(f"     No prior allocation found for this page")
                    print()
        
        if len(self.slab_double_frees) > 0:
            slab_double_free_errors = [df for df in self.slab_double_frees if df.error_type == 'double_free']
            slab_free_without_alloc_errors = [df for df in self.slab_double_frees if df.error_type == 'free_without_alloc']
            
            if slab_double_free_errors:
                print(f"\nSLAB DOUBLE FREE ERRORS ({len(slab_double_free_errors)}):")
                print("-" * 50)
                for i, error in enumerate(slab_double_free_errors, 1):
                    print(f"  {i}. Object 0x{error.obj_addr:x}")
                    print(f"     Current free: Line {error.free_line:4d} "
                          f"(cache {error.cache_name}, size {error.free_size})")
                    if error.original_alloc_line:
                        print(f"     Previous free: Line {error.original_alloc_line:4d}")
                    print()
            
            if slab_free_without_alloc_errors:
                print(f"\nSLAB FREE WITHOUT ALLOCATION ERRORS ({len(slab_free_without_alloc_errors)}):")
                print("-" * 50)
                for i, error in enumerate(slab_free_without_alloc_errors, 1):
                    print(f"  {i}. Object 0x{error.obj_addr:x}")
                    print(f"     Free attempt: Line {error.free_line:4d} "
                          f"(cache {error.cache_name}, size {error.free_size})")
                    print(f"     No prior allocation found for this object")
                    print()
    
    def print_error_analysis(self) -> None:
        """Print focused analysis of double allocation and double free errors"""
        print("=" * 60)
        print("DOUBLE ALLOCATION & DOUBLE FREE ANALYSIS")
        print("=" * 60)
        
        total_events = len(self.events) + len(self.slab_events)
        total_page_errors = len(self.double_allocations) + len(self.double_frees)
        total_slab_errors = len(self.slab_double_allocations) + len(self.slab_double_frees)
        total_errors = total_page_errors + total_slab_errors
        error_rate = (total_errors / total_events * 100) if total_events > 0 else 0
        
        print(f"\nERROR OVERVIEW:")
        print(f"  Total events processed: {total_events} ({len(self.events)} page, {len(self.slab_events)} slab)")
        print(f"  Total errors found: {total_errors} ({total_page_errors} page, {total_slab_errors} slab)")
        print(f"  Error rate: {error_rate:.2f}%")
        print()
        
        if total_errors == 0:
            print("  ✅ NO DOUBLE ALLOCATION OR DOUBLE FREE ERRORS DETECTED!")
            print("  Memory management appears to be working correctly.")
            return
        
        print(f"  ⚠️  MEMORY MANAGEMENT ERRORS DETECTED!")
        print()
        
        # Analyze page double allocations
        if self.double_allocations:
            print(f"PAGE DOUBLE ALLOCATION ANALYSIS:")
            print(f"  Count: {len(self.double_allocations)}")
            
            # Group by address to find patterns
            addr_count = Counter(error.page_addr for error in self.double_allocations)
            most_common_addr = addr_count.most_common(1)[0] if addr_count else None
            
            if most_common_addr:
                print(f"  Most problematic address: 0x{most_common_addr[0]:x} ({most_common_addr[1]} times)")
            
            # Group by order
            order_count = Counter(error.first_alloc_order for error in self.double_allocations)
            print(f"  By order: {dict(order_count)}")
            
            # Find line number patterns
            line_gaps = []
            for error in self.double_allocations:
                gap = error.second_alloc_line - error.first_alloc_line
                line_gaps.append(gap)
            
            if line_gaps:
                avg_gap = sum(line_gaps) / len(line_gaps)
                min_gap = min(line_gaps)
                max_gap = max(line_gaps)
                print(f"  Line gaps: avg={avg_gap:.1f}, min={min_gap}, max={max_gap}")
            print()
        
        # Analyze slab double allocations
        if self.slab_double_allocations:
            print(f"SLAB DOUBLE ALLOCATION ANALYSIS:")
            print(f"  Count: {len(self.slab_double_allocations)}")
            
            # Group by address
            addr_count = Counter(error.obj_addr for error in self.slab_double_allocations)
            most_common_addr = addr_count.most_common(1)[0] if addr_count else None
            
            if most_common_addr:
                print(f"  Most problematic address: 0x{most_common_addr[0]:x} ({most_common_addr[1]} times)")
            
            # Group by cache
            cache_count = Counter(error.first_alloc_cache for error in self.slab_double_allocations)
            print(f"  By cache: {dict(cache_count)}")
            
            # Group by size
            size_count = Counter(error.first_alloc_size for error in self.slab_double_allocations)
            print(f"  By object size: {dict(size_count)}")
            print()
        
        # Analyze double frees
        if self.double_frees:
            double_free_errors = [df for df in self.double_frees if df.error_type == 'double_free']
            free_without_alloc_errors = [df for df in self.double_frees if df.error_type == 'free_without_alloc']
            
            print(f"PAGE DOUBLE FREE ANALYSIS:")
            print(f"  Double frees: {len(double_free_errors)}")
            print(f"  Frees without allocation: {len(free_without_alloc_errors)}")
            
            if double_free_errors:
                # Group by address
                addr_count = Counter(error.page_addr for error in double_free_errors)
                most_common_addr = addr_count.most_common(1)[0] if addr_count else None
                
                if most_common_addr:
                    print(f"  Most problematic address: 0x{most_common_addr[0]:x} ({most_common_addr[1]} times)")
                
                # Group by order
                order_count = Counter(error.free_order for error in double_free_errors)
                print(f"  By order: {dict(order_count)}")
            print()
        
        # Analyze slab double frees
        if self.slab_double_frees:
            slab_double_free_errors = [df for df in self.slab_double_frees if df.error_type == 'double_free']
            slab_free_without_alloc_errors = [df for df in self.slab_double_frees if df.error_type == 'free_without_alloc']
            
            print(f"SLAB DOUBLE FREE ANALYSIS:")
            print(f"  Double frees: {len(slab_double_free_errors)}")
            print(f"  Frees without allocation: {len(slab_free_without_alloc_errors)}")
            
            if slab_double_free_errors:
                # Group by address
                addr_count = Counter(error.obj_addr for error in slab_double_free_errors)
                most_common_addr = addr_count.most_common(1)[0] if addr_count else None
                
                if most_common_addr:
                    print(f"  Most problematic address: 0x{most_common_addr[0]:x} ({most_common_addr[1]} times)")
                
                # Group by cache
                cache_count = Counter(error.cache_name for error in slab_double_free_errors)
                print(f"  By cache: {dict(cache_count)}")
            print()
        
        # Print detailed errors
        self._print_detailed_errors()
        
        # Recommendations
        print("RECOMMENDATIONS:")
        if self.double_allocations:
            print("  PAGE ALLOCATIONS:")
            print("  • Check page allocation logic - pages may not be properly marked as allocated")
            print("  • Review page tracking data structures for corruption")
            print("  • Add assertions in allocation code to catch double allocations early")
        
        if self.slab_double_allocations:
            print("  SLAB ALLOCATIONS:")
            print("  • Check slab allocation logic - objects may not be properly marked as allocated")
            print("  • Review slab object tracking within caches")
            print("  • Verify slab cache state management")
        
        if self.double_frees:
            print("  PAGE DEALLOCATIONS:")
            print("  • Check page deallocation logic - pages may be freed multiple times")
            print("  • Review free page tracking and ensure proper cleanup")
            print("  • Add guards against freeing already-freed pages")
        
        if self.slab_double_frees:
            print("  SLAB DEALLOCATIONS:")
            print("  • Check slab deallocation logic - objects may be freed multiple times")
            print("  • Review slab free list management")
            print("  • Add guards against freeing already-freed objects")
        
        print("  GENERAL:")
        print("  • Consider adding more detailed logging around problematic addresses")
        print("  • Review memory management synchronization if running in SMP environment")
        print("  • Add magic numbers or canaries for corruption detection")
    
    def save_error_report(self, output_file: str) -> None:
        """Save detailed error report to file"""
        with open(output_file, 'w') as f:
            f.write("DOUBLE ALLOCATION & DOUBLE FREE ERROR REPORT\n")
            f.write("=" * 60 + "\n\n")
            
            # Error summary
            total_errors = len(self.double_allocations) + len(self.double_frees)
            f.write(f"SUMMARY:\n")
            f.write(f"  Double allocations: {len(self.double_allocations)}\n")
            f.write(f"  Double frees: {len([df for df in self.double_frees if df.error_type == 'double_free'])}\n")
            f.write(f"  Frees without allocation: {len([df for df in self.double_frees if df.error_type == 'free_without_alloc'])}\n")
            f.write(f"  Total errors: {total_errors}\n\n")
            
            # Detailed double allocation errors
            if self.double_allocations:
                f.write("DOUBLE ALLOCATION ERRORS:\n")
                f.write("-" * 40 + "\n")
                for i, error in enumerate(self.double_allocations, 1):
                    f.write(f"{i:3d}. Page 0x{error.page_addr:08x}\n")
                    f.write(f"     First:  Line {error.first_alloc_line:4d} "
                           f"(order {error.first_alloc_order}, flags 0x{error.first_alloc_flags:x})\n")
                    f.write(f"     Second: Line {error.second_alloc_line:4d} "
                           f"(order {error.second_alloc_order}, flags 0x{error.second_alloc_flags:x})\n")
                    f.write(f"     Gap: {error.second_alloc_line - error.first_alloc_line} lines\n\n")
            
            # Detailed double free errors
            if self.double_frees:
                double_free_errors = [df for df in self.double_frees if df.error_type == 'double_free']
                free_without_alloc_errors = [df for df in self.double_frees if df.error_type == 'free_without_alloc']
                
                if double_free_errors:
                    f.write("DOUBLE FREE ERRORS:\n")
                    f.write("-" * 40 + "\n")
                    for i, error in enumerate(double_free_errors, 1):
                        f.write(f"{i:3d}. Page 0x{error.page_addr:08x}\n")
                        f.write(f"     Free: Line {error.free_line:4d} "
                               f"(order {error.free_order}, flags 0x{error.free_flags:x})\n")
                        if error.original_alloc_line:
                            f.write(f"     Previous free: Line {error.original_alloc_line:4d}\n")
                        f.write("\n")
                
                if free_without_alloc_errors:
                    f.write("FREE WITHOUT ALLOCATION ERRORS:\n")
                    f.write("-" * 40 + "\n")
                    for i, error in enumerate(free_without_alloc_errors, 1):
                        f.write(f"{i:3d}. Page 0x{error.page_addr:08x}\n")
                        f.write(f"     Free attempt: Line {error.free_line:4d} "
                               f"(order {error.free_order}, flags 0x{error.free_flags:x})\n\n")
            
            # Event timeline around errors
            f.write("EVENT TIMELINE AROUND ERRORS:\n")
            f.write("-" * 40 + "\n")
            error_lines = set()
            for error in self.double_allocations:
                error_lines.add(error.first_alloc_line)
                error_lines.add(error.second_alloc_line)
            for error in self.double_frees:
                error_lines.add(error.free_line)
                if error.original_alloc_line:
                    error_lines.add(error.original_alloc_line)
            
            # Show events around error lines
            for line_num in sorted(error_lines):
                f.write(f"\nEvents around line {line_num}:\n")
                start_line = max(1, line_num - 5)
                end_line = line_num + 5
                
                relevant_events = [e for e in self.events 
                                 if start_line <= e.line_number <= end_line]
                
                for event in relevant_events:
                    marker = " *** ERROR ***" if event.line_number == line_num else ""
                    f.write(f"  Line {event.line_number:4d}: {event.event_type:5s} "
                           f"0x{event.page_addr:08x} (order {event.order}, "
                           f"flags 0x{event.flags:x}){marker}\n")


def main():
    """Main function"""
    if len(sys.argv) != 2:
        print("Usage: python3 page_log_analyzer.py <log_file>")
        sys.exit(1)
    
    log_file = sys.argv[1]
    analyzer = PageAllocAnalyzer()
    
    print(f"Analyzing log file for page and slab allocations: {log_file}")
    analyzer.parse_log_file(log_file)
    
    # Print focused error analysis first
    analyzer.print_error_analysis()
    
    print("\n" + "=" * 60)
    print("FULL SUMMARY")
    print("=" * 60)
    
    # Then print full summary
    analyzer.print_summary()
    analyzer.print_leaks()
    
    # Save reports
    base_name = log_file.rsplit('.', 1)[0]
    
    # Save detailed report
    report_file = base_name + "_analysis_report.txt"
    analyzer.save_detailed_report(report_file)
    print(f"\nDetailed report saved to: {report_file}")
    
    # Save focused error report
    error_report_file = base_name + "_error_report.txt"
    analyzer.save_error_report(error_report_file)
    print(f"Error-focused report saved to: {error_report_file}")
    
    # Summary
    total_page_errors = len(analyzer.double_allocations) + len(analyzer.double_frees) + len(analyzer.page_range_validation_errors)
    total_slab_errors = len(analyzer.slab_double_allocations) + len(analyzer.slab_double_frees) + len(analyzer.slab_page_validation_errors)
    total_errors = total_page_errors + total_slab_errors
    
    if total_errors > 0:
        print(f"\n⚠️  Found {total_errors} memory management errors!")
        print(f"   PAGE ERRORS: {total_page_errors}")
        print(f"   - {len(analyzer.double_allocations)} double allocations")
        print(f"   - {len([df for df in analyzer.double_frees if df.error_type == 'double_free'])} double frees")
        print(f"   - {len([df for df in analyzer.double_frees if df.error_type == 'free_without_alloc'])} frees without allocation")
        print(f"   - {len(analyzer.page_range_validation_errors)} range violations")
        print(f"   SLAB ERRORS: {total_slab_errors}")
        print(f"   - {len(analyzer.slab_double_allocations)} double allocations")
        print(f"   - {len([df for df in analyzer.slab_double_frees if df.error_type == 'double_free'])} double frees")
        print(f"   - {len([df for df in analyzer.slab_double_frees if df.error_type == 'free_without_alloc'])} frees without allocation")
        print(f"   - {len(analyzer.slab_page_validation_errors)} slab validation errors")
    else:
        print(f"\n✅ No memory management errors detected!")
    
    # Save error report
    error_report_file = log_file.rsplit('.', 1)[0] + "_error_report.txt"
    analyzer.save_error_report(error_report_file)
    print(f"Error report saved to: {error_report_file}")


if __name__ == "__main__":
    main()
