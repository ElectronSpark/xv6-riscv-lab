#!/usr/bin/env python3
"""
Double Allocation & Double Free Analyzer

This script focuses specifically on detecting double allocations and double frees
in xv6-riscv kernel page and slab allocation logs.

Usage: python3 double_alloc_analyzer.py <log_file>
"""

import re
import sys
from collections import defaultdict, Counter
from dataclasses import dataclass
from typing import Dict, List, Optional


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


class DoubleAllocAnalyzer:
    """Analyzes double allocation and double free errors"""
    
    def __init__(self):
        self.events: List[PageEvent] = []
        self.slab_events: List[SlabEvent] = []
        self.allocated_pages: Dict[int, PageEvent] = {}  # page_addr -> alloc_event
        self.freed_pages: Dict[int, PageEvent] = {}  # page_addr -> last_free_event
        self.allocated_slab_objs: Dict[int, SlabEvent] = {}  # obj_addr -> alloc_event
        self.freed_slab_objs: Dict[int, SlabEvent] = {}  # obj_addr -> last_free_event
        
        # Error tracking
        self.double_allocations: List[DoubleAllocationError] = []
        self.double_frees: List[DoubleFreeError] = []
        self.slab_double_allocations: List[SlabDoubleAllocationError] = []
        self.slab_double_frees: List[SlabDoubleFreeError] = []
        
    def parse_log_file(self, filename: str) -> None:
        """Parse the log file and extract page allocation events"""
        # Regex patterns for page allocation and deallocation
        alloc_pattern = r'page_alloc: order (\d+), flags (0x[0-9a-fA-F]+), page (0x[0-9a-fA-F]+)'
        free_pattern = r'page_free: order (\d+), flags (0x[0-9a-fA-F]+), page (0x[0-9a-fA-F]+)'
        
        # Regex patterns for slab allocation and deallocation
        slab_alloc_pattern = r'slab_alloc\(\): cache ([^(]+)\(([^)]+)\), obj (0x[0-9a-fA-F]+), size: (\d+)'
        slab_free_pattern = r'slab_free\(\): cache ([^(]+)\(([^)]+)\), obj (0x[0-9a-fA-F]+), size: (\d+)'
        
        try:
            with open(filename, 'r') as f:
                for line_num, line in enumerate(f, 1):
                    line = line.strip()
                    
                    # Check for page allocation event
                    alloc_match = re.search(alloc_pattern, line)
                    if alloc_match:
                        order = int(alloc_match.group(1))
                        flags = int(alloc_match.group(2), 16)
                        page_addr = int(alloc_match.group(3), 16)
                        
                        event = PageEvent('alloc', order, flags, page_addr, line_num)
                        self.events.append(event)
                        self._check_allocation(event)
                        continue
                    
                    # Check for page deallocation event
                    free_match = re.search(free_pattern, line)
                    if free_match:
                        order = int(free_match.group(1))
                        flags = int(free_match.group(2), 16)
                        page_addr = int(free_match.group(3), 16)
                        
                        event = PageEvent('free', order, flags, page_addr, line_num)
                        self.events.append(event)
                        self._check_deallocation(event)
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
                        self._check_slab_allocation(event)
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
                        self._check_slab_deallocation(event)
                        
        except FileNotFoundError:
            print(f"❌ Error: File '{filename}' not found")
            sys.exit(1)
        except Exception as e:
            print(f"❌ Error reading file: {e}")
            sys.exit(1)
    
    def _check_allocation(self, event: PageEvent) -> None:
        """Check for double allocation errors"""
        # Calculate number of pages allocated (2^order)
        pages_count = 1 << event.order
        
        # Check each page in the allocation range
        for i in range(pages_count):
            page_addr = event.page_addr + (i * 4096)  # Assuming 4KB pages
            
            if page_addr in self.allocated_pages:
                # Double allocation detected!
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
                
                print(f"🚨 DOUBLE ALLOCATION at 0x{page_addr:x}")
                print(f"   First:  Line {prev_alloc.line_number:4d} (order {prev_alloc.order}, flags 0x{prev_alloc.flags:x})")
                print(f"   Second: Line {event.line_number:4d} (order {event.order}, flags 0x{event.flags:x})")
                print()
            
            # Record this allocation
            self.allocated_pages[page_addr] = event
    
    def _check_deallocation(self, event: PageEvent) -> None:
        """Check for double free errors"""
        # Calculate number of pages freed (2^order)
        pages_count = 1 << event.order
        
        # Check each page in the deallocation range
        for i in range(pages_count):
            page_addr = event.page_addr + (i * 4096)  # Assuming 4KB pages
            
            if page_addr not in self.allocated_pages:
                # This page is not currently allocated
                if page_addr in self.freed_pages:
                    # Double free - page was already freed
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
                    
                    print(f"🚨 DOUBLE FREE at 0x{page_addr:x}")
                    print(f"   Previous: Line {prev_free.line_number:4d} (order {prev_free.order}, flags 0x{prev_free.flags:x})")
                    print(f"   Current:  Line {event.line_number:4d} (order {event.order}, flags 0x{event.flags:x})")
                    print()
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
                    
                    print(f"🚨 FREE WITHOUT ALLOCATION at 0x{page_addr:x}")
                    print(f"   Free: Line {event.line_number:4d} (order {event.order}, flags 0x{event.flags:x})")
                    print()
            else:
                # Normal free - remove from allocated pages
                del self.allocated_pages[page_addr]
            
            # Record this free event
            self.freed_pages[page_addr] = event
    
    def _check_slab_allocation(self, event: SlabEvent) -> None:
        """Check for slab double allocation errors"""
        if event.obj_addr in self.allocated_slab_objs:
            # Double slab allocation detected!
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
            
            print(f"🚨 SLAB DOUBLE ALLOCATION at 0x{event.obj_addr:x}")
            print(f"   First:  Line {prev_alloc.line_number:4d} (cache {prev_alloc.cache_name}, size {prev_alloc.obj_size})")
            print(f"   Second: Line {event.line_number:4d} (cache {event.cache_name}, size {event.obj_size})")
            print()
        
        # Record this allocation
        self.allocated_slab_objs[event.obj_addr] = event
    
    def _check_slab_deallocation(self, event: SlabEvent) -> None:
        """Check for slab double free errors"""
        if event.obj_addr not in self.allocated_slab_objs:
            # This object is not currently allocated
            if event.obj_addr in self.freed_slab_objs:
                # Double free - object was already freed
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
                
                print(f"🚨 SLAB DOUBLE FREE at 0x{event.obj_addr:x}")
                print(f"   Previous: Line {prev_free.line_number:4d} (cache {prev_free.cache_name}, size {prev_free.obj_size})")
                print(f"   Current:  Line {event.line_number:4d} (cache {event.cache_name}, size {event.obj_size})")
                print()
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
                
                print(f"🚨 SLAB FREE WITHOUT ALLOCATION at 0x{event.obj_addr:x}")
                print(f"   Free: Line {event.line_number:4d} (cache {event.cache_name}, size {event.obj_size})")
                print()
        else:
            # Normal free - remove from allocated objects
            del self.allocated_slab_objs[event.obj_addr]
        
        # Record this free event
        self.freed_slab_objs[event.obj_addr] = event
    
    def print_analysis(self) -> None:
        """Print comprehensive double allocation/free analysis"""
        print("=" * 70)
        print("🔍 DOUBLE ALLOCATION & DOUBLE FREE ANALYSIS (PAGE & SLAB)")
        print("=" * 70)
        
        total_events = len(self.events) + len(self.slab_events)
        total_double_allocs = len(self.double_allocations)
        total_double_frees = len([df for df in self.double_frees if df.error_type == 'double_free'])
        total_free_without_alloc = len([df for df in self.double_frees if df.error_type == 'free_without_alloc'])
        
        total_slab_double_allocs = len(self.slab_double_allocations)
        total_slab_double_frees = len([df for df in self.slab_double_frees if df.error_type == 'double_free'])
        total_slab_free_without_alloc = len([df for df in self.slab_double_frees if df.error_type == 'free_without_alloc'])
        
        total_errors = total_double_allocs + len(self.double_frees) + total_slab_double_allocs + len(self.slab_double_frees)
        
        print(f"\n📊 SUMMARY:")
        print(f"   Total events processed: {total_events} ({len(self.events)} page, {len(self.slab_events)} slab)")
        print(f"   PAGE ERRORS:")
        print(f"     Double allocations: {total_double_allocs}")
        print(f"     Double frees: {total_double_frees}")
        print(f"     Frees without allocation: {total_free_without_alloc}")
        print(f"   SLAB ERRORS:")
        print(f"     Double allocations: {total_slab_double_allocs}")
        print(f"     Double frees: {total_slab_double_frees}")
        print(f"     Frees without allocation: {total_slab_free_without_alloc}")
        print(f"   Total errors: {total_errors}")
        
        if total_errors == 0:
            print(f"\n✅ EXCELLENT! No double allocation or double free errors detected!")
            print(f"   Page and slab memory management appear to be working correctly.")
            return
        
        error_rate = (total_errors / total_events * 100) if total_events > 0 else 0
        print(f"   Error rate: {error_rate:.2f}%")
        print(f"\n⚠️  MEMORY MANAGEMENT ISSUES DETECTED!")
        
        # Detailed analysis of page double allocations
        if total_double_allocs > 0:
            print(f"\n🔴 PAGE DOUBLE ALLOCATION ANALYSIS:")
            
            # Most problematic addresses
            addr_count = Counter(error.page_addr for error in self.double_allocations)
            print(f"   Affected addresses: {len(addr_count)}")
            
            if len(addr_count) <= 5:
                print(f"   All affected addresses:")
                for addr, count in addr_count.most_common():
                    print(f"     0x{addr:08x}: {count} double allocation(s)")
            else:
                print(f"   Top 5 most problematic addresses:")
                for addr, count in addr_count.most_common(5):
                    print(f"     0x{addr:08x}: {count} double allocation(s)")
            
            # Order analysis
            order_count = Counter(error.first_alloc_order for error in self.double_allocations)
            print(f"   By allocation order: {dict(sorted(order_count.items()))}")
            
            # Line gap analysis
            line_gaps = [error.second_alloc_line - error.first_alloc_line 
                        for error in self.double_allocations]
            if line_gaps:
                avg_gap = sum(line_gaps) / len(line_gaps)
                print(f"   Line gaps (avg/min/max): {avg_gap:.1f}/{min(line_gaps)}/{max(line_gaps)}")
        
        # Detailed analysis of slab double allocations
        if total_slab_double_allocs > 0:
            print(f"\n🔴 SLAB DOUBLE ALLOCATION ANALYSIS:")
            
            # Most problematic addresses
            addr_count = Counter(error.obj_addr for error in self.slab_double_allocations)
            print(f"   Affected addresses: {len(addr_count)}")
            
            if len(addr_count) <= 5:
                print(f"   All affected addresses:")
                for addr, count in addr_count.most_common():
                    print(f"     0x{addr:08x}: {count} double allocation(s)")
            else:
                print(f"   Top 5 most problematic addresses:")
                for addr, count in addr_count.most_common(5):
                    print(f"     0x{addr:08x}: {count} double allocation(s)")
            
            # Cache analysis
            cache_count = Counter(error.first_alloc_cache for error in self.slab_double_allocations)
            print(f"   By cache: {dict(cache_count)}")
            
            # Size analysis
            size_count = Counter(error.first_alloc_size for error in self.slab_double_allocations)
            print(f"   By object size: {dict(sorted(size_count.items()))}")
        
        # Detailed analysis of page double frees
        if len(self.double_frees) > 0:
            print(f"\n🔴 PAGE DOUBLE FREE ANALYSIS:")
            
            if total_double_frees > 0:
                double_free_errors = [df for df in self.double_frees if df.error_type == 'double_free']
                addr_count = Counter(error.page_addr for error in double_free_errors)
                print(f"   Double free addresses: {len(addr_count)}")
                
                if len(addr_count) <= 5:
                    for addr, count in addr_count.most_common():
                        print(f"     0x{addr:08x}: {count} double free(s)")
                else:
                    print(f"   Top 5 addresses with double frees:")
                    for addr, count in addr_count.most_common(5):
                        print(f"     0x{addr:08x}: {count} double free(s)")
            
            if total_free_without_alloc > 0:
                free_without_alloc_errors = [df for df in self.double_frees if df.error_type == 'free_without_alloc']
                addr_count = Counter(error.page_addr for error in free_without_alloc_errors)
                print(f"   Free-without-allocation addresses: {len(addr_count)}")
                
                if len(addr_count) <= 5:
                    for addr, count in addr_count.most_common():
                        print(f"     0x{addr:08x}: {count} free(s) without allocation")
                else:
                    print(f"   Top 5 addresses freed without allocation:")
                    for addr, count in addr_count.most_common(5):
                        print(f"     0x{addr:08x}: {count} free(s) without allocation")
        
        # Detailed analysis of slab double frees
        if len(self.slab_double_frees) > 0:
            print(f"\n🔴 SLAB DOUBLE FREE ANALYSIS:")
            
            if total_slab_double_frees > 0:
                double_free_errors = [df for df in self.slab_double_frees if df.error_type == 'double_free']
                addr_count = Counter(error.obj_addr for error in double_free_errors)
                print(f"   Double free addresses: {len(addr_count)}")
                
                if len(addr_count) <= 5:
                    for addr, count in addr_count.most_common():
                        print(f"     0x{addr:08x}: {count} double free(s)")
                else:
                    print(f"   Top 5 addresses with double frees:")
                    for addr, count in addr_count.most_common(5):
                        print(f"     0x{addr:08x}: {count} double free(s)")
                
                # Cache analysis for double frees
                cache_count = Counter(error.cache_name for error in double_free_errors)
                print(f"   By cache: {dict(cache_count)}")
            
            if total_slab_free_without_alloc > 0:
                free_without_alloc_errors = [df for df in self.slab_double_frees if df.error_type == 'free_without_alloc']
                addr_count = Counter(error.obj_addr for error in free_without_alloc_errors)
                print(f"   Free-without-allocation addresses: {len(addr_count)}")
                
                if len(addr_count) <= 5:
                    for addr, count in addr_count.most_common():
                        print(f"     0x{addr:08x}: {count} free(s) without allocation")
                else:
                    print(f"   Top 5 addresses freed without allocation:")
                    for addr, count in addr_count.most_common(5):
                        print(f"     0x{addr:08x}: {count} free(s) without allocation")
        
        print(f"\n💡 DEBUGGING RECOMMENDATIONS:")
        if total_double_allocs > 0:
            print(f"   🔍 Page Double Allocations:")
            print(f"     • Add assertions in page allocation code to catch double allocations")
            print(f"     • Check if page tracking data structures are corrupted")
            print(f"     • Review allocation logic around the problematic addresses")
        
        if total_slab_double_allocs > 0:
            print(f"   🔍 Slab Double Allocations:")
            print(f"     • Check slab allocation logic - objects may not be properly marked as allocated")
            print(f"     • Review slab object tracking within caches")
            print(f"     • Verify slab cache state management")
            
            # Focus on most problematic cache
            if self.slab_double_allocations:
                cache_count = Counter(error.first_alloc_cache for error in self.slab_double_allocations)
                most_common = cache_count.most_common(1)[0]
                print(f"     • Focus debugging on cache '{most_common[0]}' (appears {most_common[1]} times)")
        
        if len(self.double_frees) > 0:
            print(f"   🔍 Page Double Frees:")
            print(f"     • Add guards to prevent freeing already-freed pages")
            print(f"     • Check deallocation logic and page state tracking")
            print(f"     • Review cleanup code that might be freeing pages multiple times")
        
        if len(self.slab_double_frees) > 0:
            print(f"   🔍 Slab Double Frees:")
            print(f"     • Add guards to prevent freeing already-freed objects")
            print(f"     • Check slab deallocation logic and object state tracking")
            print(f"     • Review slab cache management and free list handling")
        
        print(f"   🔍 General:")
        print(f"     • Add more detailed logging around problematic addresses")
        print(f"     • Check for race conditions if running in SMP environment")
        print(f"     • Consider adding page/object magic numbers for corruption detection")
        print(f"     • Review memory barriers and synchronization primitives")
    
    def print_detailed_errors(self) -> None:
        """Print detailed list of all errors"""
        if len(self.double_allocations) > 0:
            print(f"\n📋 DETAILED DOUBLE ALLOCATION ERRORS:")
            print("-" * 60)
            for i, error in enumerate(self.double_allocations, 1):
                gap = error.second_alloc_line - error.first_alloc_line
                print(f"{i:3d}. Page 0x{error.page_addr:08x}")
                print(f"     First allocation:  Line {error.first_alloc_line:4d} "
                      f"(order {error.first_alloc_order}, flags 0x{error.first_alloc_flags:x})")
                print(f"     Second allocation: Line {error.second_alloc_line:4d} "
                      f"(order {error.second_alloc_order}, flags 0x{error.second_alloc_flags:x})")
                print(f"     Gap: {gap} lines")
                print()
        
        if len(self.double_frees) > 0:
            double_free_errors = [df for df in self.double_frees if df.error_type == 'double_free']
            free_without_alloc_errors = [df for df in self.double_frees if df.error_type == 'free_without_alloc']
            
            if double_free_errors:
                print(f"\n📋 DETAILED DOUBLE FREE ERRORS:")
                print("-" * 60)
                for i, error in enumerate(double_free_errors, 1):
                    print(f"{i:3d}. Page 0x{error.page_addr:08x}")
                    print(f"     Free attempt: Line {error.free_line:4d} "
                          f"(order {error.free_order}, flags 0x{error.free_flags:x})")
                    if error.original_alloc_line:
                        print(f"     Previous free: Line {error.original_alloc_line:4d}")
                    print()
            
            if free_without_alloc_errors:
                print(f"\n📋 DETAILED FREE-WITHOUT-ALLOCATION ERRORS:")
                print("-" * 60)
                for i, error in enumerate(free_without_alloc_errors, 1):
                    print(f"{i:3d}. Page 0x{error.page_addr:08x}")
                    print(f"     Free attempt: Line {error.free_line:4d} "
                          f"(order {error.free_order}, flags 0x{error.free_flags:x})")
                    print(f"     No prior allocation found")
                    print()
    
    def save_report(self, output_file: str) -> None:
        """Save detailed report to file"""
        with open(output_file, 'w') as f:
            f.write("DOUBLE ALLOCATION & DOUBLE FREE ERROR REPORT\n")
            f.write("=" * 60 + "\n\n")
            
            total_errors = len(self.double_allocations) + len(self.double_frees)
            f.write(f"SUMMARY:\n")
            f.write(f"  Double allocations: {len(self.double_allocations)}\n")
            f.write(f"  Double frees: {len([df for df in self.double_frees if df.error_type == 'double_free'])}\n")
            f.write(f"  Frees without allocation: {len([df for df in self.double_frees if df.error_type == 'free_without_alloc'])}\n")
            f.write(f"  Total errors: {total_errors}\n\n")
            
            if total_errors == 0:
                f.write("✅ NO ERRORS DETECTED - Memory management is working correctly!\n")
                return
            
            # Write all errors with context
            f.write("ALL ERRORS WITH CONTEXT:\n")
            f.write("-" * 40 + "\n\n")
            
            all_errors = []
            for error in self.double_allocations:
                all_errors.append(('double_alloc', error.second_alloc_line, error))
            for error in self.double_frees:
                all_errors.append(('double_free', error.free_line, error))
            
            # Sort by line number
            all_errors.sort(key=lambda x: x[1])
            
            for error_type, line_num, error in all_errors:
                if error_type == 'double_alloc':
                    f.write(f"DOUBLE ALLOCATION at 0x{error.page_addr:08x}:\n")
                    f.write(f"  First:  Line {error.first_alloc_line:4d} (order {error.first_alloc_order}, flags 0x{error.first_alloc_flags:x})\n")
                    f.write(f"  Second: Line {error.second_alloc_line:4d} (order {error.second_alloc_order}, flags 0x{error.second_alloc_flags:x})\n")
                    f.write(f"  Gap: {error.second_alloc_line - error.first_alloc_line} lines\n\n")
                else:
                    if error.error_type == 'double_free':
                        f.write(f"DOUBLE FREE at 0x{error.page_addr:08x}:\n")
                        f.write(f"  Free: Line {error.free_line:4d} (order {error.free_order}, flags 0x{error.free_flags:x})\n")
                        if error.original_alloc_line:
                            f.write(f"  Previous: Line {error.original_alloc_line:4d}\n")
                    else:
                        f.write(f"FREE WITHOUT ALLOCATION at 0x{error.page_addr:08x}:\n")
                        f.write(f"  Free: Line {error.free_line:4d} (order {error.free_order}, flags 0x{error.free_flags:x})\n")
                    f.write("\n")


def main():
    """Main function"""
    if len(sys.argv) != 2:
        print("Usage: python3 double_alloc_analyzer.py <log_file>")
        print("\nThis tool focuses specifically on detecting double allocations and double frees")
        print("in both page and slab memory management.")
        sys.exit(1)
    
    log_file = sys.argv[1]
    analyzer = DoubleAllocAnalyzer()
    
    print(f"🔍 Analyzing log file for double allocations and double frees: {log_file}")
    print("-" * 70)
    
    analyzer.parse_log_file(log_file)
    
    analyzer.print_analysis()
    analyzer.print_detailed_errors()
    
    # Save report
    base_name = log_file.rsplit('.', 1)[0]
    report_file = base_name + "_double_alloc_errors.txt"
    analyzer.save_report(report_file)
    
    total_page_errors = len(analyzer.double_allocations) + len(analyzer.double_frees)
    total_slab_errors = len(analyzer.slab_double_allocations) + len(analyzer.slab_double_frees)
    total_errors = total_page_errors + total_slab_errors
    
    if total_errors > 0:
        print(f"\n💾 Error report saved to: {report_file}")
        print(f"\n⚠️  RESULT: {total_errors} memory management errors found!")
        print(f"   • Page errors: {total_page_errors}")
        print(f"   • Slab errors: {total_slab_errors}")
    else:
        print(f"\n✅ RESULT: No double allocation or double free errors detected in pages or slabs!")


if __name__ == "__main__":
    main()
