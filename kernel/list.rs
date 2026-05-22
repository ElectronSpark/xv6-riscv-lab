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
            let iter = unsafe { $crate::list::ListIterator::new($head, $offset) };
            for $t in iter {
                $body
            }
        }
    };
}
