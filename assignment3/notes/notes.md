* OO-implementation, simplyfying the code logic by handling directory one layer at a time via pointer manipulation
* Passing pointers instead of the object itself (C is always passed by value)
* As in regular file-system implementation, the "directory"-part of the volume holds pointers to the free FCB's, which is mimiced in our struct dir_data, where inodes are controlled by the directories, and the base directory holds pointers to all the possible files and directories in the FUSE instance
* helper methods (allowing for more readable code, helper structs, less code duplication)
* base struct attributes: mode, time, size manipulation attributes
* dynamic and static memory allocation
* errno (existing error messages allowing for more general and readable code for C-programmers)
* Make sure to explicitly tell that we distinguish a directory from a file in our implementation, but we see the `backup`-directory as a file, as it is mentioned in the book, that files and directories may not be distuingishable in some file-system implementations
* memory-sharing, forking (allowing the parent process which is actually running the FUSE-instance to be more responsive, as it has to handle no syncing), sleeping (making sure our process for backing up is not hugging the CPU, only awokned every 10 seconds), using existing syncing tool via the `system()`-method calling the `CLI` which only touches the modified files, and syncs such that the state of FUSE and the backup is as alike as possible
* moving FUSE to disc by creating a backup, while restoring from the backup everytime our implementation is opened