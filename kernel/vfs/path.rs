//! Borrowed kernel paths, independent of C strings and filesystem storage.
//!
//! Names are byte strings: non-UTF-8 filenames are valid. Separators are
//! skipped here, while `.` and `..` remain for the VFS to resolve against
//! the current directory, process root, and mount tree.

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(super) struct KernelPath<'a> {
    bytes: &'a [u8],
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(super) enum PathError {
    Empty,
    TooLong,
    EmbeddedNul,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(super) enum ParentPath<'a> {
    Root,
    Current,
    Path(KernelPath<'a>),
}

impl<'a> KernelPath<'a> {
    pub(super) const MAX_LEN: usize = 65535;

    pub(super) fn new(bytes: &'a [u8]) -> Result<Self, PathError> {
        if bytes.is_empty() {
            return Err(PathError::Empty);
        }
        if bytes.len() > Self::MAX_LEN {
            return Err(PathError::TooLong);
        }
        // A supplied length describes the entire path. Silently stopping at
        // a NUL would let lookup and parent lookup address different files.
        if bytes.contains(&0) {
            return Err(PathError::EmbeddedNul);
        }
        Ok(Self { bytes })
    }

    pub(super) fn is_absolute(self) -> bool {
        self.bytes.starts_with(b"/")
    }

    pub(super) fn components(self) -> impl DoubleEndedIterator<Item = &'a [u8]> {
        self.bytes
            .split(|&byte| byte == b'/')
            .filter(|component| !component.is_empty())
    }

    /// Split off the final name, ignoring trailing slashes. A path made
    /// entirely of slashes denotes the root and has no final name.
    pub(super) fn parent_and_name(self) -> Option<(ParentPath<'a>, &'a [u8])> {
        let end = self.bytes.iter().rposition(|&byte| byte != b'/')? + 1;
        let name_start = self.bytes[..end]
            .iter()
            .rposition(|&byte| byte == b'/')
            .map_or(0, |index| index + 1);
        let name = &self.bytes[name_start..end];
        let parent_end = self.bytes[..name_start]
            .iter()
            .rposition(|&byte| byte != b'/')
            .map_or(0, |index| index + 1);
        let parent = if parent_end != 0 {
            ParentPath::Path(Self {
                bytes: &self.bytes[..parent_end],
            })
        } else if self.is_absolute() {
            ParentPath::Root
        } else {
            ParentPath::Current
        };
        Some((parent, name))
    }
}

#[cfg(test)]
mod tests {
    use super::{KernelPath, ParentPath, PathError};

    #[test]
    fn components_skip_slashes_without_normalizing_dot_names() {
        let path = KernelPath::new(b"///a//./../..name/b///").unwrap();
        assert!(path.is_absolute());
        let components: Vec<_> = path.components().collect();
        assert_eq!(components, [b"a".as_slice(), b".", b"..", b"..name", b"b"]);
        assert_eq!(KernelPath::new(b"////").unwrap().components().count(), 0);
    }

    #[test]
    fn parsing_is_bounded_and_accepts_non_utf8() {
        // The slice need not have a byte accessible after its end.
        let bytes = [b'a', b'/', 0xff, b'x'];
        let path = KernelPath::new(&bytes[..3]).unwrap();
        assert!(!path.is_absolute());
        assert_eq!(path.components().next_back(), Some([0xff].as_slice()));
    }

    #[test]
    fn nul_cannot_select_a_different_parent_and_child() {
        for bytes in [b"\0".as_slice(), b"a\0", b"a\0/other", b"/a/\0b"] {
            assert_eq!(KernelPath::new(bytes), Err(PathError::EmbeddedNul));
        }
        assert_eq!(KernelPath::new(b""), Err(PathError::Empty));
    }

    #[test]
    fn maximum_length_is_inclusive() {
        let bytes = vec![b'a'; KernelPath::MAX_LEN + 1];
        assert!(KernelPath::new(&bytes[..KernelPath::MAX_LEN]).is_ok());
        assert_eq!(KernelPath::new(&bytes), Err(PathError::TooLong));
    }

    #[test]
    fn parent_classifies_root_current_and_nested_paths() {
        for bytes in [b"file".as_slice(), b"file///"] {
            let path = KernelPath::new(bytes).unwrap();
            assert_eq!(
                path.parent_and_name(),
                Some((ParentPath::Current, b"file".as_slice()))
            );
        }
        for bytes in [b"/file".as_slice(), b"///file///"] {
            let path = KernelPath::new(bytes).unwrap();
            assert_eq!(
                path.parent_and_name(),
                Some((ParentPath::Root, b"file".as_slice()))
            );
        }
        for (bytes, parent) in [
            (b"a//b///".as_slice(), b"a".as_slice()),
            (b"///a//b///".as_slice(), b"///a".as_slice()),
            (b"./b", b"."),
            (b"../b", b".."),
        ] {
            let path = KernelPath::new(bytes).unwrap();
            assert_eq!(
                path.parent_and_name(),
                Some((
                    ParentPath::Path(KernelPath::new(parent).unwrap()),
                    b"b".as_slice()
                ))
            );
        }
        assert_eq!(KernelPath::new(b"////").unwrap().parent_and_name(), None);
    }
}
