"""Pretty printers for xv6 list_node_t structures."""

import gdb
import gdb.printing

_MAX_WALK = 512
_SAMPLE_LIMIT = 8

def _strip_typedefs(typ):
    try:
        return typ.strip_typedefs()
    except AttributeError:
        return typ

def _addr(value):
    if value is None:
        return None
    try:
        return int(value)
    except (gdb.error, OverflowError, ValueError, TypeError):
        return None

def _format_ptr(value):
    addr = _addr(value)
    if addr is None:
        return "<unavailable>"
    if addr == 0:
        return "0x0"
    return f"0x{addr:x}"

class ListNodePrinter:
    """Pretty printer for list_node_t."""

    def __init__(self, val):
        self.val = val

    def _node_ptr(self):
        try:
            typ = _strip_typedefs(self.val.type)
        except gdb.error:
            return None
        if typ.code == gdb.TYPE_CODE_PTR:
            return self.val
        try:
            return self.val.address
        except gdb.error:
            return None

    def _node_value(self, node_ptr):
        if node_ptr is None:
            return None
        try:
            return node_ptr.dereference()
        except gdb.error:
            return None

    def _is_self_ref(self, node_ptr, node_val):
        next_ptr = node_val['next']
        prev_ptr = node_val['prev']
        ptr_addr = _addr(node_ptr)
        return (
            ptr_addr is not None
            and _addr(next_ptr) == ptr_addr
            and _addr(prev_ptr) == ptr_addr
        )

    def _collect_nodes(self, head_ptr):
        head_val = self._node_value(head_ptr)
        if head_val is None:
            return 0, [], False
        cycle_guard = set()
        head_addr = _addr(head_ptr)
        if head_addr is not None:
            cycle_guard.add(head_addr)
        nodes = []
        count = 0
        truncated = False
        next_ptr = head_val['next']
        steps = 0
        while steps < _MAX_WALK:
            steps += 1
            addr = _addr(next_ptr)
            if addr is None:
                truncated = True
                break
            if addr == 0:
                truncated = (count != 0)
                break
            if addr in cycle_guard:
                if head_addr is not None and addr != head_addr:
                    truncated = True
                break
            cycle_guard.add(addr)
            if len(nodes) < _SAMPLE_LIMIT:
                nodes.append(next_ptr)
            count += 1
            next_val = self._node_value(next_ptr)
            if next_val is None:
                truncated = True
                break
            next_ptr = next_val['next']
        else:
            truncated = True
        return count, nodes, truncated

    def to_string(self):
        node_ptr = self._node_ptr()
        if node_ptr is None:
            return "list_node(<optimized out>)"
        node_val = self._node_value(node_ptr)
        if node_val is None:
            return f"list_node@{_format_ptr(node_ptr)} (unreadable)"
        prev_ptr = node_val['prev']
        next_ptr = node_val['next']
        label = f"list_node@{_format_ptr(node_ptr)}"
        suffix = f" prev={_format_ptr(prev_ptr)} next={_format_ptr(next_ptr)}"
        if self._is_self_ref(node_ptr, node_val):
            return label + suffix + " (detached)"
        length, sample, truncated = self._collect_nodes(node_ptr)
        if length == 0:
            return label + suffix + " (no forward links)"
        preview = ", ".join(_format_ptr(ptr) for ptr in sample)
        if truncated and sample:
            preview += ", ..."
        return (
            f"{label} len={length}" +
            (" [" + preview + "]" if sample else "")
        )

    def display_hint(self):
        return 'array'

    def children(self):
        node_ptr = self._node_ptr()
        node_val = self._node_value(node_ptr)
        if node_val is None:
            return
        yield 'prev', node_val['prev']
        yield 'next', node_val['next']


def _build_pretty_printer():
    printer = gdb.printing.RegexpCollectionPrettyPrinter("xv6")
    printer.add_printer(
        'list_node_t',
        r'^(?:struct\s+)?list_node(?:_t)?(?:\s*\*)?$',
        ListNodePrinter,
    )
    return printer


def register_printers(obj=None):
    if obj is None:
        obj = gdb
    gdb.printing.register_pretty_printer(obj, _build_pretty_printer())


register_printers()
