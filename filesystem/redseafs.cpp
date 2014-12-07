#include <fs_index.h>
#include <fs_info.h>
#include <fs_interface.h>
#include <fs_query.h>
#include <fs_volume.h>

// #include <ObjectList.h>

#include <syslog.h>
#include <stdlib.h>
#include <string.h>
#include "redsea.h"

#if 1
#define TRACE(format, ...) do { syslog(LOG_DEBUG, format, __VA_ARGS__); } while (0)
#else
#define TRACE(format, ...) do { } while (0)
#endif

#define TRACE_ENTER do { TRACE(" entered %s\n", __PRETTY_FUNCTION__); } while(0)
#define TRACE_EXIT do { TRACE(" exited  %s\n", __PRETTY_FUNCTION__); } while(0)
#define TRACE_REF_ADD(num) do { syslog(LOG_DEBUG, "    ref[%llu]++ (%s)\n", (num), __PRETTY_FUNCTION__); } while (0)
#define TRACE_REF_DEL(num) do { syslog(LOG_DEBUG, "    ref[%llu]-- (%s)\n", (num), __PRETTY_FUNCTION__); } while (0)
#define TRACE_REF_MOVE(num, reason) do { syslog(LOG_DEBUG, "exit -> [%llu] ref moves to %s\n", (num), (reason)); } while (0)
// #pragma mark - Module Interface

ino_t ino_for_dirent(fs_volume *volume, RedSeaDirEntry *entry);
ino_t ino_for_pointer(fs_volume *volume, RSEntryPointer pointer);

void enter_dirent(fs_volume *volume, RedSeaDirEntry *entry);
void release_dirent(fs_volume *volume, RedSeaDirEntry *entry);

RedSeaDirEntry *dirent_for_pointer(fs_volume *volume, RSEntryPointer pointer);

RedSeaDirEntry *dirent_for_ino(fs_volume *volume, ino_t inode)
{
	TRACE_ENTER;
	RedSeaDirEntry *returnval;

	if (get_vnode(volume, inode, (void **)(&returnval)) != B_OK)
		return NULL;

	TRACE_REF_ADD( inode);

	TRACE_EXIT;
	return returnval;
}


RedSeaDirEntry *entry_for_name(fs_volume *volume, RedSeaDirectory *directory, const char *name)
{
	TRACE_ENTER;
	TRACE("entry_for_name -> dir '%s' (%llu), name '%s'\n", directory->Name(), directory->DirEntry().mCluster, name);
	if (strcmp(".", name) == 0)
		return dirent_for_ino(volume, directory->DirEntry().mCluster);
	
	for (int i = 0; i < directory->CountEntries(); i++)
	{
		RedSeaDirEntry *entry = dirent_for_pointer(volume, directory->GetEntry(i));
		if (strcmp(entry->Name(), name) == 0) {
			TRACE_EXIT;
			return entry;
		}

		release_dirent(volume, entry);
	}
	
	TRACE_EXIT;
	return NULL;
}

static status_t
redsea_std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
		{
			openlog("readseafs", LOG_SERIAL | LOG_CONS, LOG_KERN);
			return B_OK;
		}

		case B_MODULE_UNINIT:
			return B_OK;

		default:
			return B_ERROR;
	}
}

status_t redsea_lookup(fs_volume *volume, fs_vnode *v_dir, const char *name, ino_t *id)
{
	TRACE_ENTER;
	RedSeaDirEntry *entr = (RedSeaDirEntry *)v_dir->private_node;
	if (!entr->IsDirectory())
		return B_ERROR;
	TRACE("Looking up %s in dir %s\n", name, entr->Name());
	
	RedSeaDirectory *dir = (RedSeaDirectory *)entr;
	
	RedSeaDirEntry *entry = entry_for_name(volume, dir, name);

	if (entry != NULL) {
		release_dirent(volume, entry); // extra reference
		TRACE_REF_MOVE(entry->DirEntry().mCluster, "lookup ino_t");
		*id = ino_for_dirent(volume, entry);
		return B_OK;
	}
	
	TRACE_EXIT;
	return B_ERROR;
}


status_t redsea_get_vnode_name(fs_volume *volume, fs_vnode *vnode, char *buffer, size_t bufferSize)
{
	TRACE_ENTER;
	RedSeaDirEntry *entr = (RedSeaDirEntry *)vnode->private_node;

	if (entr == NULL)
		return B_ERROR;

	strncpy(buffer, entr->Name(), bufferSize);
	TRACE_EXIT;
	return B_OK;
}


status_t redsea_unlink(fs_volume *volume, fs_vnode *v_dir, const char *name)
{
	TRACE_ENTER;
	RedSeaDirEntry *entr = (RedSeaDirEntry *)v_dir->private_node;
	if (!entr->IsDirectory())
		return B_ERROR;
	
	TRACE("Unlinking '%s' in '%s' (%llu)\n", name, entr->Name(), entr->DirEntry().mCluster);
	
	RedSeaDirectory *dir = (RedSeaDirectory *)entr;
	RedSeaDirEntry *entry = entry_for_name(volume, dir, name);

	if (entry == NULL)
		return B_ERROR;

	entry->Delete();
	entry->Flush();

	release_dirent(volume, entry);
	remove_vnode(volume, entry->DirEntry().mCluster);
	delete entry;
	TRACE_EXIT;

	return B_OK;
}


status_t redsea_rename(fs_volume *volume, fs_vnode *dir, const char *fromName,
	fs_vnode *todir, const char *toName)
{
	TRACE_ENTER;
	RedSeaDirectory *from = (RedSeaDirectory *)dir->private_node;
	RedSeaDirectory *to = (RedSeaDirectory *)todir->private_node;
	
	RedSeaDirEntry *fromnode = entry_for_name(volume, from, fromName);
	
	ino_t old_ino = ino_for_dirent(volume, fromnode);
	release_dirent(volume, fromnode);

	if (from == to) {
		from->RemoveEntry(fromnode);
		to->AddEntry(fromnode);
	} else {
		if (!to->AddEntry(fromnode)) {
			return B_ERROR;
		}
		from->RemoveEntry(fromnode);
	}

	remove_vnode(volume, old_ino);
	
	enter_dirent(volume, fromnode);
	release_dirent(volume, fromnode);

	TRACE("Renamed '%s' in '%s' (%llu) to '%s' in '%s' (%llu)\n", fromName, from->Name(), from->DirEntry().mCluster,
		toName, to->Name(), to->DirEntry().mCluster);

	TRACE_EXIT;
	return B_OK;
}


status_t redsea_access(fs_volume* volume, fs_vnode *vnode, int mode)
{
	return B_OK;
}


status_t redsea_read_stat(fs_volume *volume, fs_vnode *vnode,
	struct stat *stat)
{
	TRACE_ENTER;
	RedSeaDirEntry *entry = (RedSeaDirEntry *)vnode->private_node;
	TRACE("Read stat for vnode '%s' (%llu)\n", entry->Name(), entry->DirEntry().mCluster);
	ino_for_dirent(volume, entry); // acquire vnode
	stat->st_mode = DEFFILEMODE | (entry->IsDirectory() ? S_IFDIR : S_IFREG);
	stat->st_nlink = 0;
	stat->st_uid = 0;
	stat->st_gid = 0;
	stat->st_size = entry->DirEntry().mSize;
	stat->st_blksize = 0x200;
	stat->st_blocks = (stat->st_size + 0x1FF) / 0x200;
	TRACE_REF_MOVE(entry->DirEntry().mCluster, "stat return value");
	TRACE_EXIT;
	return B_OK;
}


status_t redsea_unmount(fs_volume *volume)
{
	/*
	RedSea *rs = (RedSea *)volume->private_volume;
	BObjectList<RedSeaDirEntry> entries;
	BObjectList<RedSeaDirectory> directoriesToTraverse;
	directoriesToTraverse.AddItem(rs->RootDirectory());
	for (int i = 0; i < directoriesToTraverse.CountItems(); i++) {
		RedSeaDirectory *dir = directoriesToTraverse.ItemAt(i);
		for (int j = 0; j < dir->CountEntries(); j++) {
			RedSeaDirEntry *entr = dir->GetEntry(j);
			if (entr->IsDirectory()) {
				directoriesToTraverse.AddItem((RedSeaDirectory *)entr);
			} else {
				entries.AddItem(entr);
			}
		}
		directoriesToTraverse.RemoveItemAt(i);
		i--;

		entries.AddItem(dir);
	}
	
	for (int i = 0; i < entries.CountItems(); i++) {
		RedSeaDirEntry *entr = entries.ItemAt(i);
		remove_vnode(volume, entr->DirEntry().mCluster);
		delete entr;
		entries.RemoveItemAt(i);
		i--;
	}*/
	
	return B_OK;
}

status_t redsea_write_stat(fs_volume *volume, fs_vnode *vnode,
	const struct stat *stat, uint32 statmask)
{
	RedSeaDirEntry *entry = (RedSeaDirEntry *)vnode->private_node;
	
	if (statmask & B_STAT_SIZE_INSECURE) {
		uint64_t origsize = entry->DirEntry().mSize;
		if (!entry->Resize(stat->st_size))
			return B_ERROR;
		entry->Flush();
		((RedSea *)volume->private_volume)->FlushBitmap();
		// TODO: Padding
	}
	
	return B_OK;
}


status_t redsea_create(fs_volume *volume, fs_vnode *dir, const char *name,
	int openmode, int perms, void **cookie, ino_t *newVnodeId)
{
	RedSeaDirectory *d = (RedSeaDirectory *)dir->private_node;
	
	RSEntryPointer p = d->CreateFile(name, 0x400);
	if (p.mLocation == gInvalidPointer.mLocation)
		return B_ERROR;

	*newVnodeId = ino_for_pointer(volume, p);
	
	return B_OK;
}

struct FileCookie {
	RedSeaFile *file;
	int openmode;
};

status_t redsea_open(fs_volume *volume, fs_vnode *vnode, int openmode, void **cookie)
{
	TRACE_ENTER;
	
	*cookie = malloc(sizeof(FileCookie));
	FileCookie *c = (FileCookie *)*cookie;

	c->openmode = openmode;	
	c->file = (RedSeaFile *)vnode->private_node;
	TRACE("Opening '%s' (%llu) -> open mode 0x%X\n", c->file->Name(), c->file->DirEntry().mCluster, openmode);
	TRACE_EXIT;
	return B_OK;
}


status_t redsea_close(fs_volume *volume, fs_vnode *vnode, void *cookie)
{
	TRACE_ENTER;
	RedSeaFile *file = (RedSeaFile *)vnode->private_node;
	TRACE("Closing '%s' (%llu)\n", file->Name(), file->DirEntry().mCluster);
	TRACE_EXIT;
	return B_OK;
}


status_t redsea_free_cookie(fs_volume *volume, fs_vnode *vnode, void *cookie)
{
	TRACE_ENTER;
	delete (FileCookie *)cookie;
	TRACE_EXIT;
	return B_OK;
}


status_t redsea_read(fs_volume *volume, fs_vnode *vnode, void *cookie,
	off_t pos, void *buffer, size_t *length)
{
	TRACE_ENTER;

	FileCookie *c = (FileCookie *) cookie;
	RedSeaFile *f = c->file;

	if (!(c->openmode & O_RDONLY) && !(c->openmode & O_RDWR))
		return B_DONT_DO_THAT;

	*length = f->Read(pos, *length, buffer);

	TRACE_EXIT;
	if (*length == UINT64_MAX)
		return B_ERROR;

	return B_OK;
}


status_t redsea_write(fs_volume *volume, fs_vnode *vnode, void *cookie,
	off_t pos, const void *buffer, size_t *length)
{
	FileCookie *c = (FileCookie *) cookie;
	RedSeaFile *f = c->file;

	if (!(c->openmode & O_WRONLY) && !(c->openmode & O_RDWR))
		return B_DONT_DO_THAT;

	*length = f->Write(pos, *length, buffer);

	if (*length == UINT64_MAX)
		return B_ERROR;

	return B_OK;
}

struct DirCookie {
	int index;
};


status_t redsea_open_dir(fs_volume *volume, fs_vnode *vnode, void **cookie) {
	RedSeaDirEntry *entr = (RedSeaDirEntry *)vnode->private_node;

	TRACE_ENTER;
	if (!entr->IsDirectory())
		return B_ERROR;

	DirCookie *c = (DirCookie *)malloc(sizeof(DirCookie));
	c->index = 0;
	*cookie = c;
	TRACE("Opening dir '%s' (%llu)\n", entr->Name(), entr->DirEntry().mCluster);
	
	TRACE_EXIT;
	return B_OK;
}


status_t redsea_close_dir(fs_volume *volume, fs_vnode *vnode, void *cookie)
{
	TRACE_ENTER;
	RedSeaDirEntry *entr = (RedSeaDirEntry *)vnode->private_node;
	TRACE("Closing dir '%s' (%llu)\n", entr->Name(), entr->DirEntry().mCluster);
	TRACE_EXIT;
	return B_OK;
}

status_t redsea_free_dir_cookie(fs_volume *volume, fs_vnode *vnode, void *cookie)
{
	TRACE_ENTER;
	delete (DirCookie *)cookie;
	TRACE_EXIT;
	return B_OK;
}


status_t redsea_read_dir(fs_volume *volume, fs_vnode *vnode, void *cookie,
	struct dirent *buffer, size_t buffersize, uint32 *num)
{
	TRACE_ENTER;
	RedSeaDirectory *dir = (RedSeaDirectory *)vnode->private_node;
	DirCookie *dircookie = (DirCookie *)cookie;
	
	if (dircookie->index >= dir->CountEntries()) {
		*num = 0;
		return B_OK;
	}
	
	RedSeaDirEntry *entry = dirent_for_pointer(volume, dir->GetEntry(dircookie->index++));
	entry->LockRead();

	buffer->d_dev = volume->id;
	buffer->d_ino = ino_for_dirent(volume, entry);

	size_t namesize = buffersize - sizeof(struct dirent) - 1;
	int namelength = strlen(entry->Name());
	buffer->d_reclen = sizeof(struct dirent) - 1 +
		(namesize > namelength ? namelength : namesize);
	
	if (namelength > namesize) {
		release_dirent(volume, entry);
		entry->UnlockRead();
		return B_BUFFER_OVERFLOW;
	}
	
	strcpy(buffer->d_name, entry->Name());
	TRACE("Reading dir %llu, got '%s' (%llu)\n", dir->DirEntry().mCluster, entry->Name(), entry->DirEntry().mCluster);

	entry->UnlockRead();
	TRACE_REF_MOVE(entry->DirEntry().mCluster, "read_dir return value");

	*num = 1;
	TRACE_EXIT;
	return B_OK;
}


status_t redsea_rewind_dir(fs_volume *volume, fs_vnode *vnode, void *cookie)
{
	TRACE_ENTER;
	RedSeaDirectory *dir = (RedSeaDirectory *)vnode->private_node;
	DirCookie *dircookie = (DirCookie *)cookie;
	dircookie->index = 0;
	
	TRACE_EXIT;
	return B_OK;
}


fs_vnode_ops gRedSeaFSVnodeOps = {
	// vnode operations
	redsea_lookup,			// lookup
	redsea_get_vnode_name,	// get name
	NULL, // write_vnode,	// write
	NULL, // remove_vnode,	// remove

	// VM file access
	NULL,					// can_page
	NULL,					// read pages
	NULL,					// write pages

	NULL,					// io?
	NULL,					// cancel io

	NULL,					// get file map

	NULL, // ioctl,
	NULL, // set_flags,
	NULL,   // NULL, // select,
	NULL,   // NULL, // deselect,
	NULL, // fsync,

	NULL, // read_symlink,
	NULL, // create_symlink,

	NULL, // link,
	redsea_unlink, // unlink,
	redsea_rename, // rename,

	redsea_access, // access,
	redsea_read_stat, // read_stat,
	redsea_write_stat, // write_stat,
	NULL,   // NULL, // preallocate,

	// file operations
	redsea_create, // create,
	redsea_open, // open,
	redsea_close, // close,
	redsea_free_cookie, // free_cookie,
	redsea_read, // read,
	redsea_write, // write,

	// directory operations
	NULL, // create_dir,
	NULL, // remove_dir,
	redsea_open_dir, // open_dir,
	redsea_close_dir, // close_dir,
	redsea_free_dir_cookie, // free_dir_cookie,
	redsea_read_dir, // read_dir,
	redsea_rewind_dir, // rewind_dir,

	// attribute directory operations
	NULL, // open_attr_dir,
	NULL, // close_attr_dir,
	NULL, // free_attr_dir_cookie,
	NULL, // read_attr_dir,
	NULL, // rewind_attr_dir,

	// attribute operations
	NULL, // create_attr,
	NULL, // open_attr,
	NULL, // close_attr,
	NULL, // free_attr_cookie,
	NULL, // read_attr,
	NULL, // write_attr,

	NULL, // read_attr_stat,
	NULL,   // NULL, // write_attr_stat,
	NULL, // rename_attr,
	NULL, // remove_attr,

	// special nodes
	NULL	// create_special_node
};


ino_t ino_for_dirent(fs_volume *volume, RedSeaDirEntry *entry)
{
	TRACE_ENTER;
	void *returnval;
	get_vnode(volume, entry->DirEntry().mCluster, &returnval);
	TRACE_REF_ADD( entry->DirEntry().mCluster);
	TRACE_EXIT;
	return entry->DirEntry().mCluster;
}


ino_t ino_for_pointer(fs_volume *volume, RSEntryPointer pointer)
{
	TRACE_ENTER;
	RedSeaDirEntry *entry = dirent_for_pointer(volume, pointer);
	TRACE_EXIT;
	return entry->DirEntry().mCluster;
}


RedSeaDirEntry *dirent_for_pointer(fs_volume *volume, RSEntryPointer pointer)
{
	TRACE_ENTER;
	RedSea *rs = (RedSea *)volume->private_volume;
	RedSeaDirEntry *returnval;

	RedSeaDirEntry *entry = rs->Create(pointer);

	uint64_t cluster = entry->DirEntry().mCluster;

	if (get_vnode(volume, cluster, (void **)(&returnval)) != B_OK) {
		publish_vnode(volume, entry->DirEntry().mCluster, entry, &gRedSeaFSVnodeOps,
			(entry->IsDirectory() ? S_IFDIR : S_IFREG), 0);
		returnval = entry;
	} else {
		delete entry;
	}
	TRACE_REF_ADD( cluster);
	TRACE_EXIT;
	return returnval;
}

void enter_dirent(fs_volume *volume, RedSeaDirEntry *entry)
{
	TRACE_ENTER;
	publish_vnode(volume, entry->DirEntry().mCluster, entry, &gRedSeaFSVnodeOps,
			(entry->IsDirectory() ? S_IFDIR : S_IFREG), 0);
	TRACE_REF_ADD( entry->DirEntry().mCluster);
	TRACE_EXIT;
}


void release_dirent(fs_volume *volume, RedSeaDirEntry *entry)
{
	TRACE_ENTER;
	TRACE_REF_DEL( entry->DirEntry().mCluster);
	put_vnode(volume, entry->DirEntry().mCluster);
	TRACE_EXIT;
}


status_t redsea_read_fs_info(fs_volume* volume, struct fs_info* info)
{
	TRACE_ENTER;
	RedSea *rs = (RedSea *)volume->private_volume;

	if (rs == NULL)
		return B_ERROR;

	RedSeaDirectory *root = (RedSeaDirectory *)dirent_for_pointer(volume, rs->RootDirectory());
	info->root = root->DirEntry().mCluster;
	
	info->flags = B_FS_IS_READONLY;
	info->block_size = 0x200;
	info->io_size = 0x200;
	info->total_blocks = rs->BootStructure().count;
	info->free_blocks = info->total_blocks - rs->UsedClusters();

	info->total_nodes = info->total_blocks * 8;
	info->free_nodes = info->free_blocks * 8;
	
	strcpy(info->volume_name, "RedSea Volume");
	strcpy(info->fsh_name, "RedSeaFS");

	TRACE_REF_MOVE(info->root, "read_fs_info return value");
	TRACE_EXIT;
	return B_OK;
}

fs_volume_ops gRedSeaFSVolumeOps = {
	redsea_unmount, // unmount,
	redsea_read_fs_info, // read_fs_info
	NULL, // write_fs_info
	NULL, // sync
	NULL, // read_vnode,

	/* index directory & index operations */
	NULL, // open_index_dir,
	NULL, // close_index_dir,
	NULL, // free_index_dir_cookie,
	NULL, // read_index_dir,
	NULL, // rewind_index_dir,

	NULL, // create_index,
	NULL, // remove_index,
	NULL, // read_index_stat,

	/* query operations */
	NULL,
	NULL,
	NULL,
	NULL,
	NULL	// rewind_query
};

uint32 redsea_get_supported_operations(partition_data *data, uint32 mask)
{
	TRACE_EXIT;
	return B_DISK_SYSTEM_SUPPORTS_WRITING;
}


status_t redsea_mount(fs_volume *volume, const char *device, uint32 flags,
	const char *args, ino_t *_rootVnodeID)
{
	TRACE_ENTER;
	int fd = open(device, O_RDWR | O_NOCACHE);

	RedSea *rs = new RedSea(fd);
	
	if (!rs->Valid()) {
		delete rs;
		return B_ERROR;
	}
	
	volume->ops = &gRedSeaFSVolumeOps;
	volume->private_volume = rs;

	RedSeaDirEntry *entry = rs->Create(rs->RootDirectory());
	publish_vnode(volume, entry->DirEntry().mCluster, entry, &gRedSeaFSVnodeOps,
			(entry->IsDirectory() ? S_IFDIR : S_IFREG), 0);

	*_rootVnodeID = entry->DirEntry().mCluster;
	TRACE_EXIT;
	return B_OK;
}


static file_system_module_info sRedSeaModuleInfo = {
	{
		"file_systems/redseafs" B_CURRENT_FS_API_VERSION,
		0,
		redsea_std_ops,
	},

	"redseafs",				// short_name
	"RedSea File System",	// pretty_name
	0						// DDM flags
	| B_DISK_SYSTEM_SUPPORTS_WRITING,

	// scanning
	NULL,	// identify_partition()
	NULL,	// scan_partition()
	NULL,	// free_identify_partition_cookie()
	NULL,	// free_partition_content_cookie()

	redsea_mount,   // mount

	redsea_get_supported_operations,	// get_supported_operations

	NULL,   // validate_resize
	NULL,   // validate_move
	NULL,   // validate_set_content_name
	NULL,   // validate_set_content_parameters
	NULL,   // validate_initialize,

	/* shadow partition modification */
	NULL,   // shadow_changed

	/* writing */
	NULL,   // defragment
	NULL,   // repair
	NULL,   // resize
	NULL,   // move
	NULL,   // set_content_name
	NULL,   // set_content_parameters
	NULL	// bfs_initialize
};

module_info *modules[] = {
	(module_info *)&sRedSeaModuleInfo,
	NULL,
};

