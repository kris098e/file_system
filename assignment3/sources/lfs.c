#include <fuse.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

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
	time_t access_time;
	time_t modify_time;
	mode_t mode;
	char *content;
	int size_allocated;
	int content_size;
};

struct dir_data
{
	char *name;
	struct dir_data *dirs;
	struct inode *files;
	int dir_count;
	int current_dir_max_size;
	int file_init_size;
	int file_count;
	int current_file_max_size;
	mode_t mode;
	time_t access_time;
	time_t modify_time;
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
static int init_root();

struct dir_data *root;

int *shared_variable;

int lfs_getattr(const char *path, struct stat *stbuf)
{
	*shared_variable = 1;
	int res = 0;
	__sync_synchronize(); // memory barrier
	printf("In getattr shared var: %d\n", *shared_variable);

	memset(stbuf, 0, sizeof(struct stat));

	struct dir_file_info *info = find_info(path);
	if (!info)
		return -ENOMEM;

	// dir
	if (info->found && info->is_dir)
	{
		struct dir_data *dir = (struct dir_data *)info->item;

		stbuf->st_mode = dir->mode;
		stbuf->st_nlink = 2 + dir->dir_count;
		stbuf->st_atime = dir->access_time;
		stbuf->st_mtime = dir->modify_time;
	}
	// file
	else if (info->found && !info->is_dir)
	{
		struct inode *file = (struct inode *)info->item;

		stbuf->st_mode = S_IFREG | 0777;
		stbuf->st_nlink = 1;
		stbuf->st_atime = file->access_time;
		stbuf->st_mtime = file->modify_time;
		stbuf->st_size = file->content_size;
	}
	else
		res = -ENOENT;

	free(info);
	return res;
}

// find whether the path is a directory or a file
// returns the info object associated with the path
static struct dir_file_info *find_info(const char *path)
{
	struct dir_data *dir = root;
	struct dir_file_info *info = malloc(sizeof(struct dir_file_info)); // return value
	if (!info)
		return NULL;

	// check if root dir
	if (strcmp(path, "/") == 0)
	{
		info->is_dir = 1;
		info->found = 1;

		info->item = root;
		return info;
	}

	info->found = 0;
	info->item = NULL;

	struct path_info *path_info = get_path_info(path);
	if (!path_info)
	{
		free(info);
		return NULL; // path_info is an errno msg in this case
	}

	char **tokens = path_info->tokens;
	int n_tokens = path_info->n_tokens;
	char *token;

	int succeded, i;

	for (i = 0; i < n_tokens; i++)
	{
		token = tokens[i];
		succeded = check_is_dir(token, &dir, info);
		if (!succeded && (i == n_tokens - 1))
		{
			check_is_file(token, dir, info, (i == n_tokens - 1));
		}
		if (succeded && (i == n_tokens - 1))
		{
			info->found = 1;
		}
	}
	free_path_info(path_info);
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
	int i = 0;

	while (i < (*dir)->dir_count && (strcmp((*dir)->dirs[i].name, token) != 0))
	{
		++i;
	}

	if (i != (*dir)->dir_count)
	{
		*dir = &(*dir)->dirs[i];
		info->is_dir = 1;
		info->item = (*dir);
		return 1;
	}

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
	struct path_info *path_info = get_path_info(path);
	if (!path_info)
		return -ENOMEM; // path_info is the errno msg at this point
	char *creation_path = path_info->creation_path;

	// find possible parent dir
	struct dir_file_info *info = find_info(creation_path);
	if (!info)
	{
		free_path_info(path_info);
		return -ENOMEM;
	}

	// if not found or not directory
	if (!info->found || !info->is_dir)
	{
		free(info);
		free_path_info(path_info);
		return -ENOENT;
	}
	// parent directory of the dir we want to delete
	struct dir_data *parent_dir = (struct dir_data *)info->item;
	// check if dir is empty
	if (parent_dir->dir_count == 0)
	{
		free_path_info(path_info);
		free(info);
		return -ENOTEMPTY;
	}

	int dir_index = 0;
	while (dir_index < parent_dir->dir_count &&
		   strcmp((parent_dir->dirs[dir_index]).name, path_info->tokens[path_info->n_tokens - 1]))
	{
		++dir_index;
	}
	// dir we want to remove
	struct dir_data *dir = &parent_dir->dirs[dir_index];

	// (dir not found in parent directory || dir contains files or dirs) -> dont free
	if (dir_index == parent_dir->dir_count || dir->dir_count != 0 || dir->file_count != 0)
	{

		free_path_info(path_info);
		free(info);
		return -ENOENT;
	}
	else
	{ // found, contains no files and no dirs

		free(dir->dirs);

		free(dir->files);
		free(dir->name);

		parent_dir->dirs[dir_index] = parent_dir->dirs[parent_dir->dir_count - 1]; // swap the last dir into the removed dirs space
		--parent_dir->dir_count;

		parent_dir->modify_time = time(NULL);
		free_path_info(path_info);

		free(info);
		return 0;
	}
}

int lfs_unlink(const char *path)
{
	struct path_info *path_info = get_path_info(path);
	if (!path_info)
		return -ENOMEM; // path_info is the errno msg at this point
	char *creation_path = path_info->creation_path;

	struct dir_file_info *info = find_info(creation_path);
	if (!info)
		return -ENOMEM;

	if (!info->found || !info->is_dir)
	{
		free(info);
		free_path_info(path_info);
		return -ENOENT;
	}
	// parent dir of file to be removed
	struct dir_data *parent_dir = (struct dir_data *)info->item;

	if (parent_dir->file_count == 0)
	{
		free_path_info(path_info);
		free(info);
		return -ENOENT;
	}
	int file_index = 0;
	while (file_index < parent_dir->file_count &&
		   strcmp((parent_dir->files[file_index]).name, path_info->tokens[path_info->n_tokens - 1]))
	{
		++file_index;
	}

	// file we want to remove
	struct inode *file = &parent_dir->files[file_index];

	// (file not found in parent directory
	if (file_index == parent_dir->file_count)
	{
		free_path_info(path_info);
		free(info);
		return -ENOENT;
	}
	else
	{ // found file
		free(file->content);
		free(file->name);
		parent_dir->files[file_index] = parent_dir->files[parent_dir->file_count - 1]; // swap the last fille into the removed files space
		--parent_dir->file_count;

		parent_dir->modify_time = time(NULL); // when removed file, update modify time
		free_path_info(path_info);
		free(info);
		return 0;
	}
}

int lfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
	struct dir_file_info *info = find_info(path);
	if (!info)
		return -ENOMEM;
	if (!info->found)
	{
		free(info);
		return -ENOENT;
	}

	if (!info->is_dir)
	{
		free(info);
		return -ENOENT;
	}
	else
	{
		struct dir_data *dir = (struct dir_data *)info->item;
		dir->access_time = time(NULL);
		for (int i = 0; i < dir->dir_count; i++)
		{
			filler(buf, dir->dirs[i].name, NULL, 0);
		}
		for (int j = 0; j < dir->file_count; j++)
		{
			filler(buf, dir->files[j].name, NULL, 0);
		}
	}
	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);

	free(info);
	return 0;
}

static int make_content(const char *path, mode_t mode, int is_file)
{
	struct path_info *path_info = get_path_info(path);
	if (!path_info)
		return -ENOMEM;
	char *creation_path = path_info->creation_path;
	char **tokens = path_info->tokens;
	int n_tokens = path_info->n_tokens;

	struct dir_file_info *info = find_info(creation_path);
	if (!info)
		return -ENOMEM;

	if (!info->found)
	{
		free_path_info(path_info);
		free(info);
		return -ENOENT;
	}

	if (is_file) // make file
	{
		struct dir_data *dir = (struct dir_data *)info->item;

		if (dir->file_count >= dir->current_file_max_size && dir->current_file_max_size * 10 < INT_MAX)
		{
			dir->files = realloc(dir->files, dir->current_file_max_size * 10 * sizeof(struct inode));
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
			free_path_info(path_info);
			free(info);
			return -ENOMEM;
		}
		dir->modify_time = time(NULL);
		free_path_info(path_info);
		free(info);
	}
	else // make dir
	{
		struct dir_data *dir = (struct dir_data *)info->item;

		if (dir->dir_count >= dir->current_dir_max_size && dir->current_dir_max_size * 10 < INT_MAX)
		{
			dir->dirs = realloc(dir->dirs, dir->current_dir_max_size * 10 * sizeof(struct dir_data));
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
		if (make_dir(dir, tokens[n_tokens - 1]) != 0)
		{
			free_path_info(path_info);
			free(info);
			return -ENOMEM;
		}
		dir->modify_time = time(NULL);
		free_path_info(path_info);
		free(info);
	}
	return 0;
}

static int make_dir(struct dir_data *dir, char *name)
{
	struct dir_data *new_dir = &dir->dirs[dir->dir_count];

	char *malloced_name = malloc(sizeof(char) * strlen(name) + 1);
	if (!malloced_name)
		return -ENOMEM;

	strcpy(malloced_name, name);

	// set fields of new dir
	new_dir->name = malloced_name;
	new_dir->dirs = malloc(sizeof(struct dir_data) * 10);
	if (!new_dir->dirs)
	{
		free(new_dir);
		free(malloced_name);
		return -ENOMEM;
	}
	new_dir->files = malloc(sizeof(struct inode) * 10);
	if (!new_dir->files)
	{
		free(malloced_name);
		free(new_dir->dirs);
		free(new_dir);
		return -ENOMEM;
	}
	new_dir->dir_count = 0;
	new_dir->current_dir_max_size = 10;
	new_dir->file_init_size = 10;
	new_dir->file_count = 0;
	new_dir->mode = S_IFDIR | 0755;
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
	struct inode *new_file = &dir->files[dir->file_count];
	char *malloced_name = malloc(sizeof(char) * strlen(name) + 1);
	if (!malloced_name)
		return -ENOMEM;

	strcpy(malloced_name, name);

	// set fields of new dir
	new_file->name = malloced_name;
	new_file->access_time = time(NULL);
	new_file->modify_time = time(NULL);
	new_file->mode = S_IFDIR | 0755;

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
	struct dir_file_info *info = find_info(path);
	if (!info)
		return -ENOMEM;

	if (info->found && !info->is_dir)
	{
		struct inode *file = (struct inode *)info->item;

		fi->fh = (uint64_t)file;
		file->access_time = time(NULL);
		return 0;
	}
	free(info);
	return -ENOENT;
}

int lfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	if (!fi->fh)
		return -ENOENT;
	struct inode *file = (struct inode *)fi->fh;

	size = (size + offset) > file->content_size ? (file->content_size - offset) : size;

	memcpy(buf, file->content + offset, size);

	// set access time when a file is read
	file->access_time = time(NULL);

	// free(file_info);
	return size;
}

int lfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	struct inode *file = (struct inode *)fi->fh;
	if (!file)
		return -ENOENT;

	file->content = realloc(file->content, size + file->content_size);
	if (!file->content)
		return -ENOMEM;
	file->content_size = size + file->content_size;

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
		if (length < file->content_size)
			file->content_size = length;
		free(info);
		return 0;
	}
}

int lfs_release(const char *path, struct fuse_file_info *fi)
{
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
	for (int i = 0; i < strlen(path); ++i)
		if (path[i] == '/')
			n_tokens++;

	char **tokens = extract_tokens(not_const_path, n_tokens);
	if (!tokens)
	{
		free(path_info);
		free(not_const_path);
		return NULL;
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

	// copy parent dir of current dir to creation_path
	memcpy(creation_path, path, (strlen(path) - strlen(tokens[n_tokens - 1]) + remove_extra_slash));

	// add null-terminating byte to creation_path
	creation_path[strlen(path) - strlen(tokens[n_tokens - 1]) + remove_extra_slash] = '\0';

	path_info->tokens = tokens;
	path_info->creation_path = creation_path;
	path_info->n_tokens = n_tokens;
	path_info->not_const_path = not_const_path;

	return path_info;
}

static void free_path_info(struct path_info *path_info)
{
	free(path_info->tokens);
	free(path_info->not_const_path);
	free(path_info->creation_path);
	free(path_info);
}

static int init_root()
{
	root = malloc(sizeof(struct dir_data));
	if (!root)
		return -ENOMEM;
	root->dir_count = 0;
	root->dirs = malloc(sizeof(struct dir_data) * 10); // base size of 10
	if (!root->dirs)
		return -ENOMEM;
	root->file_count = 0;
	root->files = malloc(sizeof(struct inode) * 10); // base size of 10
	if (!root->files)
		return -ENOMEM;
	root->name = "/";
	root->current_dir_max_size = 10;
	root->file_init_size = 10;
	root->mode = S_IFDIR | 0755;
	root->current_file_max_size = 10;
	root->access_time = time(NULL);
	root->modify_time = time(NULL);
	return 0;
}

int main(int argc, char *argv[])
{
	// int cp_error_code;
	int id, shmid;
	int err = 0;
	if (!root)
		err = init_root();

	// Create a shared memory segment
	shmid = shmget(IPC_PRIVATE, sizeof(int), 0666);
	if (shmid < 0)
	{
		err = 1;
	}

	// Attach to the shared memory segment
	shared_variable = (int *)shmat(shmid, NULL, 0);
	if (shared_variable == (int *)-1)
	{	
		err = 1;
	}

	// init shared variable
	*shared_variable = 0;

	id = fork();
	if (id == 0 && !err)
	{
		// stall to startup fuse before loading backup
		while (!(*shared_variable))
		{
			__sync_synchronize(); // memory barrier
		}

		system("mkdir ~/fusebackup 2> /dev/null");
		system("cp -r ~/fusebackup/* /tmp/fuse/ 2> /dev/null");

		while (*shared_variable)
		{
			__sync_synchronize(); // memory barrier
			system("rsync -a --delete /tmp/fuse/ ~/fusebackup/ 2> /dev/null");
			sleep(10);
		}
		exit(0);
	}
	else
	{
		fuse_main(argc, argv, &lfs_oper);
		*shared_variable = 0;
	}

	// Detach from the shared memory segment
	shmdt(shared_variable);

	// Destroy the shared memory segment
	shmctl(shmid, IPC_RMID, NULL);

	return err;
}
