It is just a matter of implementing the `get_attr` and keeping a storage of the directories, with files. By using the methods, we are then able to write, list, read, $mkdir$ and so on. The path which are given to the functions are the absolute path. Maybe make a list of  `struct dir_data`  for each dir, which has the contents of each dir, which can then be traversed.  something like

```c
struct free_list {
	int *inodes; // When done with a file_data, put the allocated inode into the list at last_index + 1. The list does not have to be sorted. 
	int *last_index;
	int size; // can be used when resizing, adding elements from the size to whatever required. 
};

struct file_data { // used like the Inode of the specific file
	int id; // Inode id?
	__time_t access_time;
	__time_t modify_time;
	char content[];
};

struct dir_data {
	struct dir_data *dirs; // could use dynamic, see https://stackoverflow.com/questions/3536153/c-dynamically-growing-array. Each dir has the option of having its own dir.

	char *name;
	char *absolutepath; // can be used for quick reference, if smart solution
	int dir_count;
	int file_count;
	
	struct file_data *files; // could use dynamic
};
```

# additional notes
have an Inode counter/tracker outside of the structure, to keep track of which inodes can be used for the files. SHould be outside of the `dir_data` struct, as all dirs needs this.

## Free file inodes
use **Binary search** 

## Directory
### data structure
#### sorted list
#### red/black tree LOL
#### Hash table (collision with linked list, or linear probing)


## Should we assign blocks of memory to each file?
### PCB block for files?

