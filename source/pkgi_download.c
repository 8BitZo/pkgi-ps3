#include "pkgi_download.h"
#include "pkgi_dialog.h"
#include "pkgi.h"
#include "pkgi_utils.h"
#include "pkgi_sha256.h"

#include <stddef.h>



#include "pdb_data.h"
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>

static char root[256];

static char resume_file[256];

static pkgi_http* http;
static const char* download_title;
static const char* download_content;
static const char* download_url;
static int download_resume;

static uint64_t initial_offset;  // where http download resumes
static uint64_t download_offset; // pkg absolute offset
static uint64_t download_size;   // pkg total size (from http request)

static sha256_ctx sha;

static void* item_file;     // current file handle
static char item_name[256]; // current file name
static char item_path[256]; // current file path
static int item_index;      // current item


// temporary buffer for downloads
static uint8_t down[64 * 1024];

// pkg header
static uint32_t index_count;
static uint64_t total_size;


// UI stuff
static char dialog_extra[256];
static char dialog_eta[256];
static uint32_t info_start;
static uint32_t info_update;

static uint32_t	task_id = 10000002;

uint32_t get_task_dir_id(void)
{
	char path[128] = "";
	int found = 0;
    struct stat sb;
	
	while (!found) {
	    pkgi_snprintf(path, sizeof(path), "/dev_hdd0/vsh/task/%d", task_id);

        if ((stat(path, &sb) == 0) && S_ISDIR(sb.st_mode)) {
	    	// there is already a directory with the ID, try again...
		    task_id++;
		} else {
		    found = 1;
		}
    }

	return task_id;
}


#define BUFF_SIZE  0x200000 // 2MB



static void write_pdb_string(const char* header, const char* pdbstr, void* fp)
{
	pkgi_write(fp, header, 4);
    
	unsigned int pdbstr_len = strlen(pdbstr) + 1;
	pkgi_write(fp, (char*) &pdbstr_len, 4);
	pkgi_write(fp, (char*) &pdbstr_len, 4);
	pkgi_write(fp, pdbstr, pdbstr_len);
}

static int create_pdb_files(void)
{
	// Create files	
	char szPDBFile[256] = "";
	char szIconFile[256] = "";
	
	pkgi_snprintf(szPDBFile, sizeof(szPDBFile), "/dev_hdd0/vsh/task/%d/d0.pdb", task_id);
	pkgi_snprintf(szIconFile, sizeof(szIconFile), "/dev_hdd0/vsh/task/%d/ICON_FILE", task_id);
	
	// write - ICON_FILE
	if (!pkgi_save(szIconFile, iconfile_data, iconfile_data_size))
	{
	    LOG("Error saving %s", szIconFile);
	    return 0;
    }
	
	void *fpPDB = pkgi_create(szPDBFile);
	if(!fpPDB)
	{
	    LOG("Failed to create file %s", szPDBFile);
		return 0;
	}

	// write - d0.pdb
	//
    pkgi_write(fpPDB, pkg_d0top_data, d0top_data_size);
	
	// 000000CE - Download expected size (in bytes)
	char dl_size[12] = {
		0x00, 0x00, 0x00, 0xCE,
		0x00, 0x00, 0x00, 0x08,
		0x00, 0x00, 0x00, 0x08
	};
	pkgi_write(fpPDB, dl_size, 12);
	pkgi_write(fpPDB, (char*) &total_size, 8);

	// 000000CB - PKG file name
	char filename_hdr[4] = { 0x00, 0x00, 0x00, 0xCB };	
	write_pdb_string(filename_hdr, root, fpPDB);

	// 000000CC - date/time
	char pkg_time_hdr[4] = { 0x00, 0x00, 0x00, 0xCC };
	write_pdb_string(pkg_time_hdr, "Mon, 11 Dec 2017 11:45:10 GMT", fpPDB);

	// 000000CA - PKG Link download URL
	char pkg_link_hdr[4] = { 0x00, 0x00, 0x00, 0xCA };
	write_pdb_string(pkg_link_hdr, download_url, fpPDB);

	// 0000006A - Icon location / path (PNG w/o extension) 
	char iconpath_hdr[4] ={ 0x00, 0x00, 0x00, 0x6A };
	write_pdb_string(iconpath_hdr, szIconFile, fpPDB);

	// 00000069 - Display title
	char title_hdr[4] = { 0x00, 0x00, 0x00, 0x69 };
	
	char title_str[256] = "";
	pkgi_snprintf(title_str, sizeof(title_str), "\xE2\x98\x85 Download \x22%s\x22", download_title);
	write_pdb_string(title_hdr, title_str, fpPDB);
	
	pkgi_write(fpPDB, pkg_d0end_data, pkg_d0end_data_size);
	pkgi_close(fpPDB);
	
	return 1;
}


static void calculate_eta(uint32_t speed)
{
    uint64_t seconds = (download_size - download_offset) / speed;
    if (seconds < 60)
    {
        pkgi_snprintf(dialog_eta, sizeof(dialog_eta), "ETA: %us", (uint32_t)seconds);
    }
    else if (seconds < 3600)
    {
        pkgi_snprintf(dialog_eta, sizeof(dialog_eta), "ETA: %um %02us", (uint32_t)(seconds / 60), (uint32_t)(seconds % 60));
    }
    else
    {
        uint32_t hours = (uint32_t)(seconds / 3600);
        uint32_t minutes = (uint32_t)((seconds - hours * 3600) / 60);
        pkgi_snprintf(dialog_eta, sizeof(dialog_eta), "ETA: %uh %02um", hours, minutes);
    }
}

static void update_progress(void)
{
    uint32_t info_now = pkgi_time_msec();
    if (info_now >= info_update)
    {
        char text[256];
        if (item_index < 0)
        {
            pkgi_snprintf(text, sizeof(text), "%s", item_name);
        }
        else
        {
            pkgi_snprintf(text, sizeof(text), "[%u/%u] %s", item_index, index_count, item_name);
        }

        if (download_resume)
        {
            // if resuming download, then there is no "download speed"
            dialog_extra[0] = 0;
        }
        else
        {
            // report download speed
            uint32_t speed = (uint32_t)(((download_offset - initial_offset) * 1000) / (info_now - info_start));
            if (speed > 10 * 1000 * 1024)
            {
                pkgi_snprintf(dialog_extra, sizeof(dialog_extra), "%u MB/s", speed / 1024 / 1024);
            }
            else if (speed > 1000)
            {
                pkgi_snprintf(dialog_extra, sizeof(dialog_extra), "%u KB/s", speed / 1024);
            }

            if (speed != 0)
            {
                // report ETA
                calculate_eta(speed);
            }
        }

        float percent;
        if (download_resume)
        {
            // if resuming, then we may not know download size yet, use total_size from pkg header
            percent = total_size ? (float)((double)download_offset / total_size) : 0.f;
        }
        else
        {
            // when downloading use content length from http response as download size
            percent = download_size ? (float)((double)download_offset / download_size) : 0.f;
        }

        pkgi_dialog_update_progress(text, dialog_extra, dialog_eta, percent);
        info_update = info_now + 500;
    }
}


static int create_dummy_pkg(void)
{	
	void *filew;
	uint32_t numr	= 0;	// bytes to save
	uint64_t fsize	= download_size;
	char *buffer 	= NULL;
	int bCopyError 	= 0;


	char szFileOut[256] ="";
	pkgi_snprintf(szFileOut, sizeof(szFileOut), "/dev_hdd0/vsh/task/%d/%s", task_id, root); 
	
	filew = pkgi_create(szFileOut);
	if(!filew)
	{
	    LOG("Failed to create file %s", szFileOut);
		return 0;
	}

	buffer = (char*)malloc(BUFF_SIZE);

	while(fsize > 0)
	{
		numr  = (fsize < BUFF_SIZE ? fsize : BUFF_SIZE);
	    fsize = fsize - numr;
		
		// write
		if(!pkgi_write(filew, buffer, numr)) 
		{
			bCopyError = 1;
			break;
		}
		download_offset += numr;
		update_progress();
	}

	if(filew) pkgi_close(filew);

	if(buffer) 
	{
		free(buffer);
		buffer = NULL;
	}
		
	if(bCopyError)
	{
		// delete file
		pkgi_rm(szFileOut);
		return 0;
	}
	
	return 1;
}


static int queue_pkg_task()
{
	char pszPKGDir[256] ="";
	pkgi_snprintf(pszPKGDir, sizeof(pszPKGDir), "/dev_hdd0/vsh/task/%d", get_task_dir_id());

	if(!pkgi_mkdirs(pszPKGDir))
	{
		pkgi_dialog_error("Could not create task directory on HDD.");
		return 0;
	}
	
    initial_offset = 0;
    LOG("requesting %s @ %llu", download_url, 0);
    http = pkgi_http_get(download_url, download_content, 0);
    if (!http)
    {
    	pkgi_dialog_error("Could not send HTTP request");
        return 0;
    }

    int64_t http_length;
    if (!pkgi_http_response_length(http, &http_length))
    {
        pkgi_dialog_error("HTTP request failed");
        return 0;
    }
    if (http_length < 0)
    {
        pkgi_dialog_error("HTTP response has unknown length");
        return 0;
    }

    download_size = http_length;
    total_size = download_size;

    if (!pkgi_check_free_space(http_length))
    {
        pkgi_dialog_error("Not enough free space on HDD");
        return 0;
    }

    LOG("http response length = %lld, total pkg size = %llu", http_length, download_size);
    info_start = pkgi_time_msec();
    info_update = pkgi_time_msec() + 500;

    pkgi_dialog_set_progress_title("Saving PKG...");
    pkgi_strncpy(item_name, sizeof(item_name), root);
    download_resume = 0;
    
    if(!create_dummy_pkg())
	{
		pkgi_dialog_error("Could not create PKG file to HDD.");
		return 0;
	}
    
	if(!create_pdb_files())
	{
		pkgi_dialog_error("Could not create PDB files to HDD.");
		return 0;
	}

	return 1;
}


static void download_start(void)
{
    LOG("resuming pkg download from %llu offset", download_offset);
    download_resume = 0;
    info_update = pkgi_time_msec() + 1000;
    pkgi_dialog_set_progress_title("Downloading");
}

static int download_data(uint8_t* buffer, uint32_t size, int save)
{
    if (pkgi_dialog_is_cancelled())
    {
        pkgi_save(resume_file, &sha, sizeof(sha));
        return 0;
    }

    update_progress();

    if (!http)
    {
        initial_offset = download_offset;
        LOG("requesting %s @ %llu", download_url, download_offset);
        http = pkgi_http_get(download_url, download_content, download_offset);
        if (!http)
        {
            pkgi_dialog_error("Could not send HTTP request");
            return 0;
        }

        int64_t http_length;
        if (!pkgi_http_response_length(http, &http_length))
        {
            pkgi_dialog_error("HTTP request failed");
            return 0;
        }
        if (http_length < 0)
        {
            pkgi_dialog_error("HTTP response has unknown length");
            return 0;
        }

        download_size = http_length + download_offset;
        total_size = download_size;

        if (!pkgi_check_free_space(http_length))
        {
            return 0;
        }

        LOG("http response length = %lld, total pkg size = %llu", http_length, download_size);
        info_start = pkgi_time_msec();
        info_update = pkgi_time_msec() + 500;
    }
        
    int read = pkgi_http_read(http, buffer, size);
    if (read < 0)
    {
        char error[256];
        pkgi_snprintf(error, sizeof(error), "HTTP download error 0x%08x", read);
        pkgi_dialog_error(error);
        pkgi_save(resume_file, &sha, sizeof(sha));
        return -1;
    }
    else if (read == 0)
    {
        pkgi_dialog_error("HTTP connection closed");
        pkgi_save(resume_file, &sha, sizeof(sha));
        return -1;
    }
    download_offset += read;

    sha256_update(&sha, buffer, read);

    if (save)
    {
        if (!pkgi_write(item_file, buffer, read))
        {
            char error[256];
            pkgi_snprintf(error, sizeof(error), "failed to write to %s", item_path);
            pkgi_dialog_error(error);
            return -1;
        }
    }
    
    return read;
}

// this includes creating of all the parent folders necessary to actually create file
static int create_file(void)
{
    char folder[256];
    pkgi_strncpy(folder, sizeof(folder), item_path);
    char* last = pkgi_strrchr(folder, '/');
    *last = 0;

    if (!pkgi_mkdirs(folder))
    {
        char error[256];
        pkgi_snprintf(error, sizeof(error), "cannot create folder %s", folder);
        pkgi_dialog_error(error);
        return 1;
    }

    LOG("creating %s file", item_name);
    item_file = pkgi_create(item_path);
    if (!item_file)
    {
        char error[256];
        pkgi_snprintf(error, sizeof(error), "cannot create file %s", item_name);
        pkgi_dialog_error(error);
        return 0;
    }

    return 1;
}

static int resume_partial_file(void)
{
    LOG("resuming %s file", item_name);
    item_file = pkgi_append(item_path);
    if (!item_file)
    {
        char error[256];
        pkgi_snprintf(error, sizeof(error), "cannot resume file %s", item_name);
        pkgi_dialog_error(error);
        return 0;
    }

    return 1;
}

static int download_pkg_file(void)
{
    LOG("downloading %s", root);

    int result = 0;

    pkgi_strncpy(item_name, sizeof(item_name), root);
    pkgi_snprintf(item_path, sizeof(item_path), "%s/%s", PKGI_PKG_FOLDER, root);

    if (download_resume)
    {
        download_offset = pkgi_get_size(item_path);
        if (!resume_partial_file()) goto bail;
        download_start();
    }
    else
    {
        if (!create_file()) goto bail;
    }

    total_size = sizeof(down);//download_size;
//    while (size > 0)
    while (download_offset != total_size)
    {
        uint32_t read = (uint32_t)min64(sizeof(down), total_size - download_offset);
        int size = download_data(down, read, 1);
        
        if (size <= 0)
        {
            goto bail;
        }
    }

    LOG("%s downloaded", item_path);
    result = 1;

bail:
    if (item_file != NULL)
    {
        pkgi_close(item_file);
        item_file = NULL;
    }
    return result;
}

static int check_integrity(const uint8_t* digest)
{
    if (!digest)
    {
        LOG("no integrity provided, skipping check");
        return 1;
    }

    uint8_t check[SHA256_DIGEST_SIZE];
    sha256_finish(&sha, check);

    LOG("checking integrity of pkg");
    if (!pkgi_memequ(digest, check, SHA256_DIGEST_SIZE))
    {
        LOG("pkg integrity is wrong, removing %s & resume data", item_path);

        pkgi_rm(item_path);
        pkgi_rm(resume_file);

        pkgi_dialog_error("pkg integrity failed, try downloading again");
        return 0;
    }

    LOG("pkg integrity check succeeded");
    return 1;
}

static int create_rap(const char* contentid, const uint8_t* rap)
{
    LOG("creating %s.rap", contentid);
    pkgi_dialog_update_progress("Creating RAP file", NULL, NULL, 1.f);

    char path[256];
    pkgi_snprintf(path, sizeof(path), "%s/%s.rap", PKGI_RAP_FOLDER, contentid);

    if (!pkgi_save(path, rap, PKGI_RAP_SIZE))
    {
        char error[256];
        pkgi_snprintf(error, sizeof(error), "Cannot save %s.rap", contentid);
        pkgi_dialog_error(error);
        return 0;
    }

    LOG("RAP file created");
    return 1;
}

int pkgi_download(const DbItem* item, const int background_dl)
{
    int result = 0;

    pkgi_snprintf(root, sizeof(root), "%.9s.pkg", item->content + 7);
    LOG("package installation file: %s", root);

    pkgi_snprintf(resume_file, sizeof(resume_file), "%s/%.9s.resume", pkgi_get_temp_folder(), item->content + 7);
    if (pkgi_load(resume_file, &sha, sizeof(sha)) == sizeof(sha))
    {
        LOG("resume file exists, trying to resume");
        pkgi_dialog_set_progress_title("Resuming...");
        download_resume = 1;
    }
    else
    {
        LOG("cannot load resume file, starting download from scratch");
        pkgi_dialog_set_progress_title("Downloading...");
        download_resume = 0;
        sha256_init(&sha);
    }

    http = NULL;
    item_file = NULL;
    item_index = -1;
    download_size = 0;
    download_offset = 0;
    download_content = item->content;
    download_url = item->url;
    download_title = item->name;

    dialog_extra[0] = 0;
    dialog_eta[0] = 0;
    info_start = pkgi_time_msec();
    info_update = info_start + 1000;


	if (background_dl)
	{
    	if (!queue_pkg_task()) goto finish;
	}
	else
	{
	    if (!download_pkg_file()) goto finish;
	    if (!check_integrity(item->digest)) goto finish;
	}
    if (item->rap)
    {
        if (!create_rap(item->content, item->rap)) goto finish;
    }

    pkgi_rm(resume_file);
    result = 1;

finish:
    if (http)
    {
        pkgi_http_close(http);
    }

    return result;
}
