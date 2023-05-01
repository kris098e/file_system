#include <fuse.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

int lfs_getattr(const char *, struct stat *);
int lfs_readdir(const char *, void *, fuse_fill_dir_t, off_t, struct fuse_file_info *);
int lfs_open(const char *path, struct fuse_file_info *);
int lfs_read(const char *path, char *, size_t, off_t, struct fuse_file_info *);
int lfs_release(const char *path, struct fuse_file_info *fi);
int lfs_mkdir(const char *path, mode_t mode);
int lfs_mknod(const char *path, mode_t mode, dev_t dev);
int lfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
int lfs_truncate(const char *path, off_t length);
int lfs_rmdir(const char *path);
int lfs_unlink(const char *path);
int lfs_utime(const char *path, struct utimbuf *file_times);

static struct fuse_operations lfs_oper = {
	.getattr = lfs_getattr,
	.readdir = lfs_readdir,
	.mknod = lfs_mknod,
	.mkdir = lfs_mkdir,
	.unlink = lfs_unlink,
	.rmdir = lfs_rmdir,
	.truncate = lfs_truncate,
	.open = lfs_open,
	.read = lfs_read,
	.release = lfs_release,
	.write = lfs_write,
	.rename = NULL,
	.utime = lfs_utime};

struct inode
{ // used like the Inode of the specific file
	char *name;
	__time_t access_time;
	__time_t modify_time;
	__mode_t mode;
	char *content;
	int size_allocated;
	int content_size;
};

struct dir_data
{
	char *name;
	struct dir_data *dirs; // could use dynamic, see https://stackoverflow.com/questions/3536153/c-dynamically-growing-array. Each dir has the option of having its own dir.
	struct inode *files;   // could use dynamic
	int dir_count;
	int current_dir_max_size;
	int file_init_size;
	int file_count;
	int current_file_max_size;
	__mode_t mode;
	__time_t access_time;
	__time_t modify_time;
};

struct dir_file_info
{
	int is_dir;
	int found;
	void *item;
};

struct path_info
{
	char *creation_path;
	char **tokens;
	int n_tokens;
	char *not_const_path;
};
static struct dir_file_info *find_info(const char *path);
static void check_is_file(char *token, struct dir_data *dir, struct dir_file_info *info, int is_last_token);
static int check_is_dir(char *token, struct dir_data **dir, struct dir_file_info *info);
static char **extract_tokens(char *path, int n_tokens);
static int make_dir(struct dir_data *dir, char *name);
static int make_nod(struct dir_data *dir, char *name);
static int make_content(const char *path, mode_t mode, int is_file);
static struct path_info *get_path_info(const char *path);
static void free_path_info(struct path_info *path_info);

struct dir_data *root;

int lfs_getattr(const char *path, struct stat *stbuf)
{
	int res = 0;
	printf("getattr: (path=%s)\n", path);

	memset(stbuf, 0, sizeof(struct stat));

	struct dir_file_info *info = find_info(path);
	if (!info)
		return -ENOMEM;

	// dir
	if (info->found && info->is_dir)
	{
		struct dir_data *dir = (struct dir_data *)info->item;
		printf("in read dir\n");
		stbuf->st_mode = dir->mode;
		stbuf->st_nlink = 2 + dir->dir_count;
		stbuf->st_atime = dir->access_time;
		stbuf->st_mtime = dir->modify_time;
	}
	// file
	else if (info->found && !info->is_dir)
	{
		printf("in read file\n");
		struct inode *file = (struct inode *)info->item;

		stbuf->st_mode = __S_IFREG | 0777;
		stbuf->st_nlink = 1;
		stbuf->st_atime = file->access_time;
		stbuf->st_mtime = file->modify_time;
		stbuf->st_size = file->content_size;
	}
	else
		res = -ENOENT;

	printf("before free info\n");
	free(info);
	printf("can return\n");
	printf("res: %d\n", res);
	return res;
}

// find whether the path is a directory or a file
// returns the info object associated with the path
static struct dir_file_info *find_info(const char *path)
{
	printf("path in find_info start: %s\n", path);
	struct dir_data *dir = root;
	struct dir_file_info *info = malloc(sizeof(struct dir_file_info)); // return value
	if (!info)
		return NULL;

	printf("1\n");
	// check if root dir
	if (strcmp(path, "/") == 0)
	{
		info->is_dir = 1;
		info->found = 1;

		info->item = root;
		return info;
	}
	printf("path in find_info 4: %s\n", path);

	printf("4\n");
	info->found = 0;
	info->item = NULL;

	printf("5\n");
	struct path_info *path_info = get_path_info(path);
	if (!path_info) {
		free(info);
		return NULL; // path_info is an errno msg in this case
	}

	char **tokens = path_info->tokens;
	int n_tokens = path_info->n_tokens;
	char *token;

	int succeded, i;

	printf("Path for tokens: %s\n", path);
	for (int k = 0; k < n_tokens; k++)
	{
		printf("token[%i] = %s\n", k, tokens[k]);
	}

	printf("6.1\n");
	for (i = 0; i < n_tokens; i++)
	{
		printf("6.2\n");
		token = tokens[i];
		succeded = check_is_dir(token, &dir, info);
		printf("6.3\n");
		if (!succeded && (i == n_tokens - 1))
		{
			printf("6.4\n");
			check_is_file(token, dir, info, (i == n_tokens - 1));
		}
		if (succeded && (i == n_tokens - 1))
		{
				info->found = 1;
		}
		printf("6.5\n");
	}
	free_path_info(path_info);
	printf("7\n");
	return info;
}

static void check_is_file(char *token, struct dir_data *dir, struct dir_file_info *info, int is_last_token)
{
	if (is_last_token)
	{
		int j = 0;
		while (j < dir->file_count && (strcmp(dir->files[j].name, token) != 0))
		{
			++j;
		}
		// found file
		if (j < dir->file_count)
		{
			info->is_dir = 0;
			info->found = 1;
			info->item = &dir->files[j];
		}
	}
}

/*
 * returns 1 if found, 0 if not.
 */
static int check_is_dir(char *token, struct dir_data **dir, struct dir_file_info *info)
{
	printf("20. before checking %s\n", (*dir)->name);
	int i = 0;
	printf("6.1.1\n");

	printf("name: %s\n", (*dir)->name);
	printf("token: %s\n", token);
	printf("dir count: %d\n", (*dir)->dir_count);

	while (i < (*dir)->dir_count && (strcmp((*dir)->dirs[i].name, token) != 0))
	{
		printf("LOOK AT ME MUM; name: %s\n", (*dir)->dirs[i].name);
		++i;
	}
	printf("6.1.2\n");

	if (i != (*dir)->dir_count)
	{
		*dir = &(*dir)->dirs[i];
		printf("found directory\n");
		printf("dir name: %s\n", (*dir)->name);
		info->is_dir = 1;
		info->item = (*dir);
		return 1;
	}
	printf("6.1.3\n");

	return 0;
}

static char **extract_tokens(char *path, int n_tokens)
{
	char **result = malloc(sizeof(char *) * n_tokens);
	if (!result)
	{
		return NULL;
	}
	char *token = strtok(path, "/");
	int i = 0;
	while (token != NULL)
	{
		result[i] = token;
		token = strtok(NULL, "/");
		++i;
	}
	return result;
}

int lfs_mkdir(const char *path, mode_t mode)
{
	return make_content(path, mode, 0);
}

int lfs_mknod(const char *path, mode_t mode, dev_t dev)
{
	return make_content(path, mode, 1);
}

int lfs_rmdir(const char *path)
{
	printf("IN RMDIR BITCH\n");
	struct path_info *path_info = get_path_info(path);
	if (!path_info)
		return -ENOMEM; // path_info is the errno msg at this point
	char *creation_path = path_info->creation_path;
	printf("rmdir 1\n");

	// find possible parent dir
	struct dir_file_info *info = find_info(creation_path);
	if (!info)
	{
		free_path_info(path_info);
		return -ENOMEM;
	}
	printf("rmdir 2\n");


	// if not found or not directory
	if (!info->found || !info->is_dir)
	{
		free(info);
		free_path_info(path_info);
		return -ENOENT;
	}
	printf("rmdir 3\n");

	// parent directory of the dir we want to delete 
	struct dir_data *parent_dir = (struct dir_data *)info->item;
	// check if dir is empty
	if (parent_dir->dir_count == 0) {
		free_path_info(path_info);
		free(info);
		return -ENOENT;
	}
	printf("rmdir 4\n");


	int dir_index = 0;
	while (dir_index < parent_dir->dir_count &&
		   strcmp((parent_dir->dirs[dir_index]).name, path_info->tokens[path_info->n_tokens-1]))
	{	
		printf("check %s == %s, dir_index = %d, with cmp = %d\n", parent_dir->dirs[dir_index].name, path_info->tokens[path_info->n_tokens-1], dir_index, strcmp((parent_dir->dirs[dir_index]).name, path_info->tokens[path_info->n_tokens-1]));
		++dir_index;
	}
	// dir we want to remove
	struct dir_data *dir = &parent_dir->dirs[dir_index];
	printf("rmdir 5\n");


	// (dir not found in parent directory || dir contains files or dirs) -> dont free
	if (dir_index == parent_dir->dir_count || dir->dir_count != 0 || dir->file_count != 0) {
		printf("rmdir 5.1\n");

		free_path_info(path_info);
		free(info);
		return -ENOENT;
	} else { // found, contains no files and no dirs
		printf("rmdir 5.2\n");

		free(dir->dirs);
		printf("rmdir 5.3\n");

		free(dir->files);
		printf("rmdir 5.4\n");
		free(dir->name);
		printf("rmdir 5.5\n");
		printf("rmdir 5.6\n");

		parent_dir->dirs[dir_index] = parent_dir->dirs[parent_dir->dir_count-1]; // swap the last dir into the removed dirs space
		--parent_dir->dir_count;
		printf("rmdir 5.7\n");

		free_path_info(path_info);
		printf("rmdir 5.8\n");

		free(info);

		printf("OUT OF RMDIR BOIIIIES\n");
		return 0;
	}
}

int lfs_unlink(const char *path)
{	
	printf("IN UNLINK\n");
	struct path_info *path_info = get_path_info(path);
	if (!path_info)
		return -ENOMEM; // path_info is the errno msg at this point
	char *creation_path = path_info->creation_path;
	printf("unlink 0\n");

	struct dir_file_info *info = find_info(creation_path);
	if (!info) return -ENOMEM;
	
	printf("unlink 1\n");

	if (!info->found || !info->is_dir)
	{
		free(info);
		free_path_info(path_info);
		return -ENOENT;
	}
	// parent dir of file to be removed
	struct dir_data *parent_dir = (struct dir_data *)info->item;
	printf("unlink 2\n");

	if (parent_dir->file_count == 0) {
		free_path_info(path_info);
		free(info);
		printf("unlink filecount == 0\n");
		return -ENOENT;
	}
	printf("unlink 3\n");
	int file_index = 0;
	while (file_index < parent_dir->file_count &&
		   strcmp((parent_dir->files[file_index]).name, path_info->tokens[path_info->n_tokens-1]))
	{
		printf("check %s == %s, dir_index = %d, with cmp = %d\n", parent_dir->files[file_index].name, path_info->tokens[path_info->n_tokens-1], file_index, strcmp((parent_dir->files[file_index]).name, path_info->tokens[path_info->n_tokens-1]));

		++file_index;
	}
	printf("unlink 4\n");

	// file we want to remove
	struct inode *file = &parent_dir->files[file_index];

	// (file not found in parent directory 
	if (file_index == parent_dir->file_count) {
		free_path_info(path_info);
		free(info);
		return -ENOENT;
	} else { // found file 
		free(file->content);
		free(file->name);
		parent_dir->files[file_index] = parent_dir->files[parent_dir->file_count-1]; // swap the last fille into the removed files space
		--parent_dir->file_count;
		
		free_path_info(path_info);
		free(info);
		printf("OUT OF UNLINK\n");
		return 0;
	}
}

int lfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
	// (void) offset;
	// (void) fi;
	printf("readdir: (path=%s)\n", path);

	struct dir_file_info *info = find_info(path);
	if (!info)
		return -ENOMEM;
	if (!info->found)
	{
		free(info);
		return -ENOENT;
	}

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);

	if (!info->is_dir)
	{
		free(info);
		return -ENOENT;
	}
	else
	{
		struct dir_data *dir = (struct dir_data *)info->item;
		printf("1. dir has name %s\n", dir->name);
		printf("2. dir has count %d\n", dir->dir_count);
		for (int i = 0; i < dir->dir_count; i++)
		{
			printf("found a dir with name %s\n", dir->dirs[i].name);
			filler(buf, dir->dirs[i].name, NULL, 0);
		}
		for (int j = 0; j < dir->file_count; j++)
		{
			filler(buf, dir->files[j].name, NULL, 0);
		}
	}
	printf("Returning from readdir\n");
	free(info);
	return 0;
}

static int make_content(const char *path, mode_t mode, int is_file)
{
	printf("IN MAKE CONTENT:\n");
	printf("PATH IN MAKE CONTENT: %s\n", path);

	struct path_info *path_info = get_path_info(path);
	if (!path_info)
		return -ENOMEM;
	char *creation_path = path_info->creation_path;
	char **tokens = path_info->tokens;
	int n_tokens = path_info->n_tokens;

	printf("creationpath: %s\n", creation_path);

	struct dir_file_info *info = find_info(creation_path);
	if (!info)
		return -ENOMEM;

	if (!info->found)
	{
		free_path_info(path_info);
		free(info);
		printf("info couldn't be found\n");
		return -ENOENT;
	}

	printf("4. \n");
	printf("did find %d\n", info->found);
	printf("MKDIR INFO FOUND %s\n", ((struct dir_data *)(info->item))->name);
	printf("tokens %s\n", tokens[0]);

	if (is_file) // make file
	{
		struct dir_data *dir = (struct dir_data *)info->item;

		// check if the file is name is already used for a dir
		// if(check_is_dir(tokens[n_tokens-1], &dir, info)) {
		// 	free_path_info(path_info);
		// 	free(info);
		// 	printf("IS DIR MAKKER!\n");
		// 	return -EISDIR;
		// }

		printf("5.1222 \n");

		if (dir->file_count >= dir->current_file_max_size && dir->current_file_max_size * 10 < INT_MAX)
		{
			dir->files = realloc(dir->files, dir->current_file_max_size * 10);
			if (!dir->files)
			{
				free_path_info(path_info);
				free(info);
				return -ENOMEM;
			}
			dir->current_file_max_size = dir->current_file_max_size * 10;
		}
		else if (dir->file_count >= dir->current_file_max_size && dir->current_file_max_size * 10 > INT_MAX)
		{
			free_path_info(path_info);
			free(info);
			return -ENOMEM;
		}

		if (make_nod(dir, tokens[n_tokens - 1]) != 0)
		{
			printf("left make_dir\n");
			free_path_info(path_info);
			free(info);
			return -ENOMEM;
		}
		printf("7. \n");
		free_path_info(path_info);
		free(info);
		printf("returning in mkdir");
	}
	else // make dir
	{
		struct dir_data *dir = (struct dir_data *)info->item;
		printf("5. \n");

		if (dir->dir_count >= dir->current_dir_max_size && dir->current_dir_max_size * 10 < INT_MAX)
		{
			printf("IN IF on line 318\n");
			dir->dirs = realloc(dir->dirs, dir->current_dir_max_size * 10);
			if (!dir->dirs)
			{
				free_path_info(path_info);
				free(info);
				return -ENOMEM;
			}
			dir->current_dir_max_size = dir->current_dir_max_size * 10;
		}
		else if (dir->dir_count >= dir->current_dir_max_size && dir->current_dir_max_size * 10 > INT_MAX)
		{
			free_path_info(path_info);
			free(info);

			return -ENOMEM;
		}
		printf("6. \n");

		if (make_dir(dir, tokens[n_tokens - 1]) != 0)
		{
			free_path_info(path_info);
			free(info);
			return -ENOMEM;
		}
		printf("7. \n");

		free_path_info(path_info);
		free(info);
		printf("returning in mkdir");
	}
	printf("LEFT MAKE CONTENT:\n");
	return 0;
}

static int make_dir(struct dir_data *dir, char *name)
{
	printf("Enter make_dir\n");
	struct dir_data *new_dir = &dir->dirs[dir->dir_count];

	char *malloced_name = malloc(sizeof(char) * strlen(name) + 1);
	if (!malloced_name) return -ENOMEM;
	
	strcpy(malloced_name, name);

	// set fields of new dir
	new_dir->name = malloced_name;
	new_dir->dirs = malloc(sizeof(struct dir_data) * 10);
	if (!new_dir->dirs)
	{
		free(new_dir);
		free(malloced_name);
		printf("new_dir->dirs couldn't be allocated\n");
		return -ENOMEM;
	}
	new_dir->files = malloc(sizeof(struct inode) * 10);
	if (!new_dir->files)
	{
		free(malloced_name);
		free(new_dir->dirs);
		free(new_dir);
		printf("new_dir->files couldn't be allocated\n");
		return -ENOMEM;
	}
	new_dir->dir_count = 0;
	new_dir->current_dir_max_size = 10;
	new_dir->file_init_size = 10;
	new_dir->file_count = 0;
	new_dir->mode = __S_IFDIR | 0755;
	new_dir->current_file_max_size = 10;
	new_dir->access_time = time(NULL);
	new_dir->modify_time = time(NULL);

	// insert new dir in parent dir
	dir->dirs[dir->dir_count] = *new_dir;
	dir->dir_count++;
	return 0;
}

static int make_nod(struct dir_data *dir, char *name)
{
	printf("Enter make_file\n");
	struct inode *new_file = &dir->files[dir->file_count];
	
	printf("Enter make_file\n");

	char *malloced_name = malloc(sizeof(char) * strlen(name) + 1);
	if (!malloced_name) return -ENOMEM;
	
	strcpy(malloced_name, name);

	// set fields of new dir
	new_file->name = malloced_name;
	new_file->access_time = time(NULL);
	new_file->modify_time = time(NULL);
	new_file->mode = __S_IFDIR | 0755;

	new_file->content = NULL;
	new_file->size_allocated = dir->file_init_size;
	new_file->content_size = 0;

	// insert new file in parent dir
	dir->files[dir->file_count] = *new_file;
	dir->file_count++;

	return 0;
}

// Permission
int lfs_open(const char *path, struct fuse_file_info *fi)
{
	printf("Opening path: %s\n", path);
	struct dir_file_info *info = find_info(path);
	if (!info)
		return -ENOMEM;

	if (info->found && !info->is_dir)
	{
		struct inode *file = (struct inode *)info->item;
		// might type cast file obj? to uint64_t
		fi->fh = (uint64_t)file;
		file->access_time = time(NULL);
		return 0;
	}
	printf("Leaving open\n");
	free(info);
	return -ENOENT;
}

int lfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	printf("read: (path=%s)\n", path);

	struct inode *file = (struct inode *)fi->fh;
	if (!file)
		return -ENOENT;

	printf("Reading file, name: %s, content: %s\n", file->name, file->content);

	size = (size + offset) > file->content_size ? file->content_size : size;

	memcpy(buf, file->content + offset, file->content_size - offset);

	// set access time when a file is read
	file->access_time = time(NULL);

	// free(file_info);
	return size;
}

int lfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	printf("write: (path=%s)\n", path);

	struct inode *file = (struct inode *)fi->fh;
	if (!file)
		return -ENOENT;

	file->content = realloc(file->content, size);
	if (!file->content)
		return -ENOMEM;
	file->content_size = size;
	printf("realloc file size bro!!\n");

	// file->content + offset to append new content
	memcpy(file->content + offset, buf, size);

	// write to file; set modified time
	file->modify_time = time(NULL);

	return size;
}

int lfs_truncate(const char *path, off_t length)
{
	struct dir_file_info *info = find_info(path);
	if (!info)
		return -ENOMEM;
	if (!info->found)
	{
		free(info);
		return -ENOENT;
	}

	printf("truncate length is: %li\n", length);
	if (info->is_dir)
	{
		free(info);
		return -ENOENT;
	}
	else
	{
		struct inode *file = (struct inode *)info->item;
		file->content = realloc(file->content, length);
		file->size_allocated = length;
		if(length < file->content_size)
			file->content_size = length;
		free(info);
		return 0;
	}
}

int lfs_release(const char *path, struct fuse_file_info *fi)
{
	printf("release: (path=%s)\n", path);
	fi->fh = 0;
	return 0;
}

int lfs_utime(const char *path, struct utimbuf *times)
{
	struct dir_file_info *info = find_info(path);
	if (!info)
		return -ENOMEM;
	if (!info->found)
	{
		free(info);
		return -ENOENT;
	}

	if (info->is_dir)
	{
		struct dir_data *dir = (struct dir_data *)info->item;
		dir->access_time = times->actime;
		dir->modify_time = times->modtime;
	}
	else
	{
		struct inode *file = (struct inode *)info->item;
		file->access_time = times->actime;
		file->modify_time = times->modtime;
	}
	return 0;
}

static struct path_info *get_path_info(const char *path)
{
	struct path_info *path_info = malloc(sizeof(struct path_info));
	if (!path_info)
		return NULL;

	int n_tokens = 0;

	char *not_const_path = malloc(sizeof(char) * strlen(path) + 1);

	if (!not_const_path)
		return NULL;

	strcpy(not_const_path, path);
	not_const_path[strlen(path)] = '\0';
	printf("get_path_info not_const_path: %s\n", not_const_path);
	for (int i = 0; i < strlen(path); ++i)
		if (path[i] == '/')
			n_tokens++;

	char **tokens = extract_tokens(not_const_path, n_tokens);
	if (!tokens)
	{
		free(path_info);
		free(not_const_path);
		printf("token couldn't be allocated\n");
		return NULL;
	}
	printf("get_path_info n_tokens: %d\n", n_tokens);
	for (int k = 0; k < n_tokens; k++) {
		printf("get_path_info tokens[%d]: %s\n", k, tokens[k]);
	}

	// behave differently when making directories in root
	// when given mkdir path an extra '/' is given if not in root. If in root the extra '/' is not given
	int remove_extra_slash = n_tokens == 1 ? 0 : -1;
	
	// Add space for terminating byte, and get parent dir of current dir
	// /foo/bar - /bar = /foo + '\0' = /foo\0
	char *creation_path = malloc((strlen(path) - strlen(tokens[n_tokens - 1]) + 1 + remove_extra_slash) * sizeof(char));
	if (!creation_path)
	{
		free(path_info);
		free(tokens);
		free(not_const_path);

		return NULL;
	}

	// copy parent dir of current dir over in creation_path
	memcpy(creation_path, path, (strlen(path) - strlen(tokens[n_tokens - 1]) + remove_extra_slash));
	
	// add null-terminating byte to creation_path
	creation_path[strlen(path) - strlen(tokens[n_tokens - 1]) + remove_extra_slash] = '\0';

	printf("Length of creation_path(%s) = %ld\n", creation_path ,strlen(creation_path));

	path_info->tokens = tokens;
	path_info->creation_path = creation_path;
	path_info->n_tokens = n_tokens;
	path_info->not_const_path = not_const_path;

	return path_info;
}

static void free_path_info(struct path_info *path_info)
{
	free(path_info->tokens);
	printf("Free 1 done\n");
	free(path_info->not_const_path);
	printf("Free 2 done\n");
	free(path_info->creation_path);
	printf("Free 3 done\n");
	free(path_info);
	printf("Free 4 done\n");
}

int main(int argc, char *argv[])
{
	root = malloc(sizeof(struct dir_data));
	if(!root) return -ENOMEM;
	root->dir_count = 0;
	root->dirs = malloc(sizeof(struct dir_data) * 10); // base size of 10
	if(!root->dirs) return -ENOMEM;
	root->file_count = 0;
	root->files = malloc(sizeof(struct inode) * 10); // base size of 10
	if(!root->files) return -ENOMEM;
	root->name = "/";
	root->current_dir_max_size = 10;
	root->file_init_size = 10;
	root->mode = __S_IFDIR | 0755;
	root->current_file_max_size = 10;
	root->access_time = time(NULL);
	root->modify_time = time(NULL);

	fuse_main(argc, argv, &lfs_oper);

	return 0;
}