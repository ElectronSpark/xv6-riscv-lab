//! Doubly intrusive list implementation in Rust.
//!
//! Replaces legacy C loop macros with a unified Iterator implementation
//! and macro wrapper.

use core::ffi::c_void;
use core::marker::PhantomData;
use crate::bindings::list_node_t;

pub struct ListIterator<'a, T> {
    head: *mut list_node_t,
    curr: *mut list_node_t,
    offset: usize,
    _marker: PhantomData<&'a T>,
}

impl<'a, T> ListIterator<'a, T> {
    /// Create a new iterator over the list.
    ///
    /// # Safety
    /// * `head` must be a valid pointer to an initialized `list_node_t`.
    /// * `offset` must be the byte offset of the `list_node_t` member within structures of type `T`.
    pub unsafe fn new(head: *mut list_node_t, offset: usize) -> Self {
        Self {
            head,
            curr: if head.is_null() { head } else { (*head).next },
            offset,
            _marker: PhantomData,
        }
    }
}

impl<'a, T> Iterator for ListIterator<'a, T> {
    type Item = *mut T;

    fn next(&mut self) -> Option<Self::Item> {
        if self.head.is_null() || self.curr.is_null() || self.curr == self.head {
            return None;
        }
        // SAFETY: `self.curr`/`self.offset` uphold the same contract as
        // `ListIterator::new`'s `# Safety` (valid list node, correct
        // `list_node_t` member offset within `T`) for every node in the
        // list, since `new` established it for `head` and this loop
        // only ever advances `curr` along that same list's `next` links.
        unsafe {
            let item_ptr = (self.curr as *mut u8).sub(self.offset) as *mut T;
            self.curr = (*self.curr).next;
            Some(item_ptr)
        }
    }
}

#[macro_export]
macro_rules! list_for_each {
    ($head:expr, $offset:expr, $t:ident, $body:block) => {
        if !$head.is_null() {
            // SAFETY: caller of `list_for_each!` upholds `ListIterator::new`'s
            // `# Safety` contract ($head is a valid, initialized `list_node_t`;
            // $offset is the correct member offset within the element type).
            let iter = unsafe { $crate::list::ListIterator::new($head, $offset) };
            for $t in iter {
                $body
            }
        }
    };
}

// ---------------------------------------------------------------------------
// Host-test suite (`cargo test --target x86_64-unknown-linux-gnu`; see
// `kernel/lib.rs`'s "Host-test seam" doc).
//
// Reference-coverage note: `test/src/ut_list_main.c`'s 14 cmocka cases
// exercise `kernel/inc/list.h`'s C macros (`list_node_push_back/front`,
// `list_node_pop_back/front`, `list_find_{first,last,next,prev}`,
// `LIST_IS_EMPTY`) -- a much wider surface (push/pop/find/detach-in-place)
// than this Rust module provides. `list.rs` only ever ported the read-only
// traversal half of that API (`ListIterator` + the `list_for_each!` macro
// wrapper); it has no push/pop/find/detach of its own (those still live in
// the C header and are exercised by the still-green `ut_list` C suite,
// unaffected by this addition -- see `docs/rustify/test_port_plan.md`).
// The suite below instead covers `ListIterator`'s own contract: correct
// traversal order over a manually-linked circular list, empty/null-head
// handling, termination (no infinite loop on a circular structure), and
// `container_of`-style payload recovery through a non-zero member offset.
#[cfg(test)]
mod tests {
    use super::*;

    /// Intrusive-list payload used by every test below. `node` is
    /// deliberately not the first field, so `offset()` below is nonzero and
    /// exercises `ListIterator`'s pointer-offset subtraction rather than a
    /// degenerate offset-0 case.
    #[repr(C)]
    struct TestItem {
        value: i32,
        node: list_node_t,
    }

    fn offset() -> usize {
        core::mem::offset_of!(TestItem, node)
    }

    fn empty_node() -> list_node_t {
        list_node_t { prev: core::ptr::null_mut(), next: core::ptr::null_mut() }
    }

    /// Link `items` into a circular list with `head` as the sentinel (not
    /// itself a payload): `head.next` is the first item, `head.prev` the
    /// last, matching the convention every ported C-ABI list consumer in
    /// this crate already relies on.
    ///
    /// `items` is linked in place and must not be resized/reallocated by the
    /// caller afterward -- every `node_ptr` below points directly into its
    /// backing storage.
    fn link_circular(head: &mut list_node_t, items: &mut [TestItem]) {
        let head_ptr = head as *mut list_node_t;
        head.next = head_ptr;
        head.prev = head_ptr;

        let mut prev = head_ptr;
        for item in items.iter_mut() {
            let node_ptr = &mut item.node as *mut list_node_t;
            // SAFETY: `prev` and `node_ptr` are both live, exclusively-owned
            // nodes for the duration of this test (either the stack-local
            // `head`, or an element of the caller's `items` slice, which by
            // this function's contract is not resized/reallocated afterward).
            unsafe {
                (*prev).next = node_ptr;
                (*node_ptr).prev = prev;
            }
            prev = node_ptr;
        }
        // SAFETY: see above.
        unsafe {
            (*prev).next = head_ptr;
        }
        head.prev = prev;
    }

    fn make_items(values: &[i32]) -> std::vec::Vec<TestItem> {
        values.iter().map(|&value| TestItem { value, node: empty_node() }).collect()
    }

    #[test]
    fn iterator_over_empty_list_yields_nothing() {
        let mut head = empty_node();
        let mut items = make_items(&[]);
        link_circular(&mut head, &mut items);

        // SAFETY: `head` is a valid, initialized, self-looped `list_node_t`
        // for the duration of this call; `offset()` is `TestItem`'s real
        // `node` field offset.
        let visited: std::vec::Vec<*mut TestItem> =
            unsafe { ListIterator::<TestItem>::new(&mut head as *mut list_node_t, offset()) }
                .collect();
        assert!(visited.is_empty());
    }

    #[test]
    fn iterator_over_null_head_yields_nothing() {
        // SAFETY: `ListIterator::new` explicitly documents a null `head` as
        // the one case where `curr` is never dereferenced.
        let visited: std::vec::Vec<*mut TestItem> =
            unsafe { ListIterator::<TestItem>::new(core::ptr::null_mut(), offset()) }.collect();
        assert!(visited.is_empty());
    }

    #[test]
    fn iterator_visits_the_single_element_in_a_one_item_list() {
        let mut head = empty_node();
        let mut items = make_items(&[42]);
        link_circular(&mut head, &mut items);

        // SAFETY: see `iterator_over_empty_list_yields_nothing`.
        let visited: std::vec::Vec<i32> =
            unsafe { ListIterator::<TestItem>::new(&mut head as *mut list_node_t, offset()) }
                .map(|p| unsafe { (*p).value })
                .collect();
        assert_eq!(visited, std::vec![42]);
    }

    #[test]
    fn iterator_visits_elements_in_link_order_head_next_to_head_prev() {
        let mut head = empty_node();
        let mut items = make_items(&[1, 2, 3, 4, 5]);
        link_circular(&mut head, &mut items);

        // SAFETY: see `iterator_over_empty_list_yields_nothing`.
        let visited: std::vec::Vec<i32> =
            unsafe { ListIterator::<TestItem>::new(&mut head as *mut list_node_t, offset()) }
                .map(|p| unsafe { (*p).value })
                .collect();
        assert_eq!(visited, std::vec![1, 2, 3, 4, 5]);
    }

    #[test]
    fn iterator_terminates_at_head_instead_of_looping_forever() {
        // Regression guard: a buggy iterator that failed to stop upon
        // `curr == head` would spin forever over this circular structure.
        let mut head = empty_node();
        let mut items = make_items(&[1, 2, 3]);
        link_circular(&mut head, &mut items);

        // SAFETY: see `iterator_over_empty_list_yields_nothing`.
        let count =
            unsafe { ListIterator::<TestItem>::new(&mut head as *mut list_node_t, offset()) }
                .count();
        assert_eq!(count, 3);
    }

    #[test]
    fn container_of_style_offset_recovers_the_exact_payload() {
        // `TestItem::node` is not at offset 0 -- confirm the fixture
        // actually exercises the subtraction rather than a degenerate case.
        assert_ne!(offset(), 0, "test fixture must use a non-zero member offset");

        let mut head = empty_node();
        let mut items = make_items(&[99]);
        link_circular(&mut head, &mut items);
        let expected_ptr = &items[0] as *const TestItem;

        // SAFETY: see `iterator_over_empty_list_yields_nothing`.
        let recovered =
            unsafe { ListIterator::<TestItem>::new(&mut head as *mut list_node_t, offset()) }
                .next()
                .unwrap();
        assert_eq!(recovered as *const TestItem, expected_ptr);
        assert_eq!(unsafe { (*recovered).value }, 99);
    }

    #[test]
    fn list_for_each_macro_visits_every_element_exactly_once() {
        let mut head = empty_node();
        let mut items = make_items(&[10, 11, 12]);
        link_circular(&mut head, &mut items);

        let head_ptr: *mut list_node_t = &mut head as *mut list_node_t;
        let mut total = 0;
        let mut visits = 0;
        crate::list_for_each!(head_ptr, offset(), item, {
            // Explicit type ascription: `list_for_each!` infers its
            // iterator's item type purely from how the loop variable is
            // used, and a bare `(*item).value` on an untyped raw pointer
            // isn't enough context by itself.
            let item: *mut TestItem = item;
            total += unsafe { (*item).value };
            visits += 1;
        });
        assert_eq!(visits, 3);
        assert_eq!(total, 10 + 11 + 12);
    }
}
