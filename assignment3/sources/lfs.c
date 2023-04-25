#include <fuse.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

int lfs_getattr( const char *, struct stat * );
int lfs_readdir( const char *, void *, fuse_fill_dir_t, off_t, struct fuse_file_info * );
int lfs_open( const char *, struct fuse_file_info * );
int lfs_read( const char *, char *, size_t, off_t, struct fuse_file_info * );
int lfs_release(const char *path, struct fuse_file_info *fi);
int lfs_mkdir(const char *path, mode_t mode);


static struct fuse_operations lfs_oper = {
	.getattr	= lfs_getattr,
	.readdir	= lfs_readdir,
	.mknod = NULL,
	.mkdir = lfs_mkdir,
	.unlink = NULL,
	.rmdir = NULL,
	.truncate = NULL,
	.open	= NULL,
	.read	= NULL,
	.release = NULL,
	.write = NULL,
	.rename = NULL,
	.utime = NULL
};



struct inode { // used like the Inode of the specific file
	char *name;
	__time_t access_time;
	__time_t modify_time;
	__mode_t mode;
	char *content;
	int size;
};
	
struct dir_data {
	char *name;
	struct dir_data *dirs; // could use dynamic, see https://stackoverflow.com/questions/3536153/c-dynamically-growing-array. Each dir has the option of having its own dir.
	struct inode *files; // could use dynamic
	int dir_count;
	int dir_max_size;
	int file_max_size;
	int file_count;
	__mode_t mode;
};

struct dir_file_info {
	int is_dir;
	int found;
	void *item;
};

struct dir_data *root;

char *strtok( char *str, const char *delim );
struct dir_file_info *find_info(const char *path);
void check_is_file(char *token, struct dir_data *dir, struct dir_file_info *info, int is_last_token);
int check_is_dir(char *token, struct dir_data *dir, struct dir_file_info *info, int is_last_token);
char **extract_tokens(char *path, int n_tokens);
char *strcat(char *dest, const char *src);
int make_dir(struct dir_data *dir, char *name, mode_t mode);


int lfs_getattr( const char *path, struct stat *stbuf ) {
	int res = 0;
	printf("getattr: (path=%s)\n", path);

	memset(stbuf, 0, sizeof(struct stat));
	/* find struct dir_data for the path  */


	if(!root) printf("root is null\n");
	printf("root dir1 %s\n", root->dirs[0].name);
	
	struct dir_file_info *info = find_info(path);
	if (!info) {
		return -ENOMEM;
	} 
	
	// dir
	if( info->found && info->is_dir) {
		struct dir_data *dir = (struct dir_data*) info->item;
		printf("in read dir\n");
		//stbuf->st_mode = __S_IFDIR | 0755;
		stbuf->st_mode = dir->mode;
		stbuf->st_nlink = 2 + dir->dir_count;
	// file
	} else if(info->found && !info->is_dir) {
		printf("in read file\n");
		struct inode *file = (struct inode*)info->item;
		
		stbuf->st_mode = __S_IFREG | 0777;
		stbuf->st_nlink = 1;
		stbuf->st_atime = file->access_time;
		stbuf->st_mtime = file->modify_time;
		stbuf->st_size = file->size;
	} else
		res = -ENOENT;

	free(info);
	printf("can return\n");
	printf("res: %d\n", res);
	return res;
}

// find whether the path is a directory or a file
// returns the info object associated with the path 
struct dir_file_info *find_info(const char *path) {
	printf("using find_info\n");
	struct dir_data *dir = root;
	struct dir_file_info *info = malloc(sizeof(struct dir_file_info));  // return value
	if (!info) {
		return NULL;
	}
	char *not_const_path = malloc(sizeof(char) * strlen(path));
	if (!not_const_path) {
		free(info);
		return NULL;
	}
	strcpy(not_const_path, path);

	// check if root dir 
	if (strcmp(path, "/") == 0) {
		info->is_dir = 1;
		info->found = 1;
		info->item = root;
		return info;
	}
	info->found = 0;
	info->item = NULL;
	int n_tokens = 0;

	for (int i = 0; i < strlen(path); ++i) 
		if (path[i] == '/') n_tokens++;

	

	char **tokens = extract_tokens(not_const_path, n_tokens);
	if (!tokens) {
		free(not_const_path);
		free(info);
		return NULL;
	}
	
	char *token;

	int succeded, i; 	

	if (tokens) {
		for(i = 0; i < n_tokens; i++ ) {
			token = tokens[i];
			succeded = check_is_dir(token, dir, info, (i == n_tokens-1));
			if(!succeded) { 
				check_is_file(token, dir, info, (i == n_tokens-1));
			}	
		}	
	}
	
	free(not_const_path);
	free(tokens);
	return info;
}

void check_is_file(char *token, struct dir_data *dir, struct dir_file_info *info, int is_last_token) {
	if (is_last_token) {
		int j = 0;
		while (j < dir->file_count && (strcmp(dir->files[j].name, token) != 0)) {
			++j;
		}
		// found file 
		if (j < dir->file_count) {
			info->is_dir = 0;
			info->found = 1;
			info->item = &dir->files[j];
		}
	}
}


int check_is_dir(char *token, struct dir_data *dir, struct dir_file_info *info, int is_last_token) {
	int i = 0;
	while (i < dir->dir_count && (strcmp(dir->dirs[i].name, token) != 0)) {
		++i;
	}
	
	if (i != dir->dir_count) {
		dir = &dir->dirs[i];
		if (is_last_token) {
			info->is_dir = 1;
			info->found = 1;
			info->item = dir;
			return 1;
		}
	}
	return 0;
}

char **extract_tokens(char *path, int n_tokens) {
	char **result = malloc(sizeof(char *) * n_tokens);
	if (!result) {
		return NULL;
	}
	char *token = strtok(path, "/");
	int i = 0;
	while (token != NULL) {
		result[i] = token;
		token = strtok(NULL, " ");
		++i;
	}
	return result;
}


int lfs_readdir( const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi ) {
	// (void) offset;
	// (void) fi;
	printf("readdir: (path=%s)\n", path);
	
	struct dir_file_info *info = find_info(path);
	if (!info) {
		return -ENOMEM;
	} 
	
	
	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);
	
	if (!info->is_dir) {
		free(info);
		return -ENOENT;

	} else {
		struct dir_data *dir = (struct dir_data *) info->item;
		printf("1. dir has name %s\n", dir->name);
		printf("2. dir has count %d\n", dir->dir_count);
		for (int i = 0; i < dir->dir_count; i++) {
			printf("found a dir with name %s\n", dir->dirs[i].name);
			filler(buf, dir->dirs[i].name, NULL, 0);
		}
		for (int j = 0; j < dir->file_count; j++) {
			filler(buf, dir->files[j].name, NULL, 0);
		}
	}
	
	free(info);
	return 0;
}

int lfs_mkdir(const char *path, mode_t mode) {
	int i, j, n_tokens;
	n_tokens = 0;

	printf("IN MKDIR: %s\n", path);

	char *not_const_path = malloc(sizeof(char) * strlen(path) + 1);
	if (!not_const_path) {
		return -ENOMEM;
	}
	printf("1. \n");
	
	strcpy(not_const_path, path);
	printf("2. \n");
	
	for (i = 0; i < strlen(path); ++i)
		if (path[i] == '/') n_tokens++;
	
	printf("3. \n");
	printf("ntokens %d\n", n_tokens);

	char **tokens = extract_tokens(not_const_path, n_tokens);
	if (!tokens) {
		free(not_const_path);
		return -ENOMEM;
	}

	printf("TOKENS: %s\n", tokens[0]);

	char *creation_path = malloc((sizeof(char)) * (strlen(path) - strlen(tokens[n_tokens-1])));
	if (!creation_path) {
		printf("asdasd\n");
		free(tokens);
		free(not_const_path);
		return -ENOMEM;
	}
	printf("123\n");
	memcpy(creation_path, path, strlen(path) - strlen(tokens[n_tokens-1]));

	printf("creationpath: %s\n", creation_path);

	struct dir_file_info *info = find_info(creation_path);
	if (!info) {
		free(creation_path);
		free(tokens);
		free(not_const_path);
		return -ENOMEM;
	}

	printf("4. \n");

	printf("MKDIR INFO FOUND %s\n", ((struct dir_data *) (info->item))->name);
	printf("tokens %s\n", tokens[0]);


	if (!info->is_dir) {
		free(info);
		free(creation_path);
		return -ENOENT;
	} else {
		struct dir_data *dir = (struct dir_data *) info->item;
		if (dir->dir_count >= dir->dir_max_size && dir->dir_count < INT_MAX) {
			if (!realloc(dir->dirs, dir->dir_max_size*10)) {
				free(creation_path);
				free(info);
				return -ENOMEM;
			}
			dir->dir_max_size = dir->dir_max_size*10;
		}  
		if (make_dir(dir, tokens[n_tokens], mode) != 0) {
			free(creation_path);
			free(info);
			return -ENOMEM;
		}

		free(creation_path);
		free(info);
		return 0;
	} 
}

int make_dir(struct dir_data *dir, char *name, mode_t mode) {

	struct dir_data *new_dir = malloc(sizeof(struct dir_data));
	if (!new_dir) {
		return -ENOMEM;
	}
	
	// set fields of new dir
	new_dir->name = name; 
	new_dir->dirs = malloc(sizeof(struct dir_data *) * 10);
	if (!new_dir->dirs) {
		printf("line 348-ish failed\n");
		free(new_dir);
		return -ENOMEM;
	}
	new_dir->files = malloc(sizeof(struct inode *) * 10);
	if (!new_dir->files) {
		free(new_dir->dirs);
		free(new_dir);
		printf("line 356-ish failed\n");
		return -ENOMEM;
	}
	new_dir->dir_count = 0;
	new_dir->dir_max_size = 10;
	new_dir->file_max_size = 10;
	new_dir->file_count = 0;
	new_dir->mode = mode;
	
	// insert new dir in parent dir
	dir->dir_count++;
	dir->dirs[dir->dir_count] = *new_dir;
	return 0;
}




//Permission
int lfs_open( const char *path, struct fuse_file_info *fi ) {
    printf("open: (path=%s)\n", path);
	return 0;
}




int lfs_read( const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi ) {
    printf("read: (path=%s)\n", path);
	struct dir_file_info *file_info = find_info(path);
	if (!file_info) {
		return -ENOMEM;
	}

	if(!file_info->found || file_info->is_dir) return -ENOENT;	
	
	struct inode *file = (struct inode *)file_info->item;
	size = size > file->size ? file->size : size;

	memcpy(buf, file->content, size);
	
	free(file_info);
	return 6;
}

int lfs_release(const char *path, struct fuse_file_info *fi) {
	printf("release: (path=%s)\n", path);
	return 0;
}

int main( int argc, char *argv[] ) {
	root = malloc(sizeof(struct dir_data));
	root->dir_count = 1;
	root->dirs = malloc(sizeof(struct dir_data*) * 10); // base size of 10
	root->file_count = 0;
	root->files = malloc(sizeof(struct inode*) * 10); // base size of 10
	root->name = "/";
	root->mode = __S_IFDIR | 0755;

	root->dirs[0].name = "hej";
	root->dirs[0].dir_count = 1;

	fuse_main( argc, argv, &lfs_oper );

	return 0;
}