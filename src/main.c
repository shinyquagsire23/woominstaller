/*
 *  woomïnstaller - Homebrew package installer for Wii U
 *
 *  Copyright (C) 2016          SALT
 *  Copyright (C) 2016          Max Thomas (Shiny Quagsire) <mtinc2@gmail.com>
 *
 *  This code is licensed under the terms of the GNU LGPL, version 2.1
 *  see file LICENSE.md or https://www.gnu.org/licenses/lgpl-2.1.txt
 */

#include <coreinit/core.h>
#include <coreinit/debug.h>
#include <coreinit/thread.h>
#include <coreinit/filesystem.h>
#include <coreinit/foreground.h>
#include <coreinit/screen.h>
#include <coreinit/internal.h>
#include <coreinit/mcp.h>
#include <proc_ui/procui.h>
#include <sysapp/launch.h>
#include <vpad/input.h>

#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "miniz.h"
#include "ezxml.h"
#include "draw.h"
#include "memory.h"

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

typedef struct InstallDevice
{
    int deviceID;
    int deviceNum;
    char *deviceName;
} InstallDevice;

FSClient *fsClient;

FSCmdBlock *fsCmd;

bool buttonState = false;
bool lastButtonState = false;

bool screenSwap = false;
bool isAppRunning = true;
bool initialized = false;
int scrollPos = 0;
int selectedFile = 0;

int selectedInstallTarget = 0;
int numDevices = 0;
int numInstallDevices = 0;
MCPDeviceList *devicelist;
InstallDevice installDevices[32];

struct dirent **directoryRead;
char *currentDirectory;
char *currentIOSDirectory;
int numEntries;
int dirLevel = 0;

#define INSTALL_QUEUE_SIZE 0x100
char **installQueue;
u8 *installQueueTarget;
char *currentlyInstalling;
bool installing = false;
MCPInstallProgress *mcp_prog_buf;
int mcp_handle;

void clearScreen(u8 r, u8 g, u8 b, u8 a)
{
    for (int ii = 0; ii < 2; ii++)
    {
        fillScreen(r,g,b,a);
        flipBuffers();
    }
}

void screenInit()
{
    //Grab the buffer size for each screen (TV and gamepad)
    int buf0_size = OSScreenGetBufferSizeEx(0);
    int buf1_size = OSScreenGetBufferSizeEx(1);
    OSReport("Screen sizes %x, %x\n", buf0_size, buf1_size);
    
    //Set the buffer area.
    screenBufferTop = MEM1_alloc(buf0_size, 0x100);
    screenBufferBottom = MEM1_alloc(buf1_size, 0x100);
    OSReport("Allocated screen buffers at %x, %x\n", screenBufferTop, screenBufferBottom);

    OSScreenSetBufferEx(0, screenBufferTop);
    OSScreenSetBufferEx(1, screenBufferBottom);
    OSReport("Set screen buffers\n");

    OSScreenEnableEx(0, 1);
    OSScreenEnableEx(1, 1);
    
    clearScreen(0,0,0,0);
}

void screenDeinit()
{
    for(int ii = 0; ii < 2; ii++)
    {
        fillScreen(0,0,0,0);
        flipBuffers();
    }
    
    MEM1_free(screenBufferTop);
    MEM1_free(screenBufferBottom);
}

void SaveCallback()
{
    OSSavesDone_ReadyToRelease(); // Required
}

bool AppRunning()
{
   if(!OSIsMainCore())
   {
      ProcUISubProcessMessages(true);
   }
   else
   {
      ProcUIStatus status = ProcUIProcessMessages(true);
    
      if(status == PROCUI_STATUS_EXITING)
      {
          // Being closed, deinit things and prepare to exit
          isAppRunning = false;
          
          if(initialized)
          {
              initialized = false;
              screenDeinit();
              memoryRelease();
              FSShutdown();
          }
          
          ProcUIShutdown();
      }
      else if(status == PROCUI_STATUS_RELEASE_FOREGROUND)
      {
          // Free up MEM1 to next foreground app, etc.
          initialized = false;
          
          screenDeinit();
          memoryRelease();
          ProcUIDrawDoneRelease();
      }
      else if(status == PROCUI_STATUS_IN_FOREGROUND)
      {
         // Reallocate MEM1, reinit screen, etc.
         if(!initialized)
         {
            initialized = true;
            
            memoryInitialize();
            screenInit();
         }
      }
   }

   return isAppRunning;
}

int readDirectory(char *path, struct dirent** out)
{
    selectedFile = 0;
    scrollPos = 0;
    
    int entryNum = 0;
    DIR *dir;
    struct dirent *ent;
    
    OSReport("Opening %s\n", path);
    dir = opendir(path);
    if(dir == NULL)
        return 0;

    while (ent = readdir(dir))
    {
        memcpy(out[entryNum], ent, sizeof(struct dirent));
        
        if(out[entryNum]->d_type == DT_DIR)
            entryNum++;
    }
    
    rewinddir(dir);
    while (ent = readdir(dir))
    {
        memcpy(out[entryNum], ent, sizeof(struct dirent));
        
        printf("%x\n", out[entryNum]->d_type);
        
        if(out[entryNum]->d_type != DT_DIR)
            entryNum++;
    }
    
    closedir(dir);
    
    return entryNum;
}

void clear_dir(char *path)
{
    DIR *dir;
    struct dirent *ent;
    
    OSReport("Clearing %s\n", path);
    
    dir = opendir(path);
    if(dir == NULL)
        return 0;
        
    OSReport("Opened %s\n", path);

    while (ent = readdir(dir))
    {
        char temp_buf[0x200];
        sprintf(temp_buf, "%s%s", path, ent->d_name);
        remove(temp_buf);
        OSReport("Removing %s\n", temp_buf);
    }
    
    closedir(dir);
    
    remove(path);
}

void moveUpDirectory()
{
    // Backtrack stdlib directory
    currentDirectory[strlen(currentDirectory)-1] = 0;
    int len = strlen(currentDirectory)-1;
    
    for(int i = len; i >= 0; i--)
    {
        if(currentDirectory[i] == '/')
            break;
        
        currentDirectory[i] = 0;
    }
    
    // Backtrack IOS directory
    currentIOSDirectory[strlen(currentIOSDirectory)-1] = 0;
    len = strlen(currentIOSDirectory)-1;
    
    for(int i = len; i >= 0; i--)
    {
        if(currentIOSDirectory[i] == '/')
            break;
        
        currentIOSDirectory[i] = 0;
    }
}

void shiftBackInstallQueue()
{
    for(int i = 1; i < INSTALL_QUEUE_SIZE; i++)
    {
        installQueue[i-1] = installQueue[i];
        installQueueTarget[i-1] = installQueueTarget[i];
        if(installQueue[i] == NULL)
            break;
    }
}

void addToInstallQueue(char *path)
{
    OSReport("Attempting to add %s to the install queue...", path);
    if(currentlyInstalling != NULL)
    {
        if(!strcmp(currentlyInstalling, path))
        {
            OSReport("%s is being installed", path);
            return;
        }
    }
        
        
    for(int i = 0; i < INSTALL_QUEUE_SIZE; i++)
    {
        if(installQueue[i] != NULL)
        {
            if(!strcmp(installQueue[i], path))
            {
                OSReport("%s is already in the install queue", path);
                return;
            }
        }
        else
        {
            OSReport("Adding %s to the install queue.", path);
            installQueue[i] = memalign(0x40, strlen(path));
            strcpy(installQueue[i], path);
            installQueueTarget[i] = selectedInstallTarget;
            break;
        }
    }
}

mz_zip_archive woomy_archive = {0};
ezxml_t woomy_xml;
bool has_icon = false;
void *icon_mem;
int woomy_install_index = 0;
int woomy_extract_prog = 0;
int woomy_extract_total = 0;
bool woomy_processing = false;
bool woomy_extracting = false;

char *woomy_archive_name;
char *woomy_entry_name;

void processInstallQueue(int argc, const char **argv)
{
    //We want that priority for unpacking
    OSSetThreadPriority(OSGetCurrentThread(), 0);

    int mcp_thread_handle = MCP_Open();
    MCPInstallProgress *mcp_install = memalign(0x40, 0x24);
    void *mcp_info_buf = memalign(0x40, 0x27F);
    void *mcp_install_buf = memalign(0x40, 0x27F);
    icon_mem = malloc(0x10100);

    OSReport("Install queue process thread started.\n");
    while(isAppRunning)
    {
        //Don't completely murder IOS with progress requests here, 
        //MCP crashes if you ping it too much apparently.
        OSSleepTicks(50000000);
        
        int ret = MCP_InstallGetProgress(mcp_thread_handle, mcp_install);
            
        //Fake info for decaf
        if(ret == 0x12345678)
        {
            mcp_install->inProgress = false;
            mcp_install->sizeProgress = 5;
            mcp_install->sizeTotal = 11;
            mcp_install->contentsProgress = 17;
            mcp_install->contentsTotal = 30;
        }
        
        if(!mcp_install->inProgress)
        {
            installing = false;
            
            //Free queue allocations
            if(currentlyInstalling != NULL)
            {
                free(currentlyInstalling);
                currentlyInstalling = NULL;
            }
            
            if(installQueue[0] == NULL)
                continue;
                
            char *to_install = installQueue[0];
            
            //If it's a file, attempt to process it
            if(to_install[strlen(to_install)-1] != '/')
            {
                if(!woomy_processing)
                {
                    woomy_extracting = false;
                    if(mz_zip_reader_init_file(&woomy_archive, to_install, 0))
                    {
                        void *meta_buf = malloc(0x8000);
                        int status = mz_zip_reader_extract_file_to_mem(&woomy_archive, "metadata.xml", meta_buf, 0x8000, 0);
                        if(!status)
                        {
                            OSReport("Install for %s failed, missing metadata.xml\n", to_install);
                            shiftBackInstallQueue();
                            continue;
                        }
                        
                        woomy_xml = ezxml_parse_str(meta_buf, strlen(meta_buf));
                            
                        ezxml_t woomy_metadata_name = ezxml_get(woomy_xml, "metadata", 0, "name", -1);
                        
                        if(!woomy_metadata_name)
                            woomy_archive_name = "<no name>";
                        else
                            woomy_archive_name = woomy_metadata_name->txt;
                            
                        //Show the icon if it's available
                        if(!strcmp(ezxml_get(woomy_xml, "metadata", 0, "icon", -1)->txt, "1"))
                        {
                            status = mz_zip_reader_extract_file_to_mem(&woomy_archive, "icon.tga", icon_mem, 0x10100, 0);
                            if(status)
                                has_icon = true;
                        }
                    }
                    else
                    {
                        OSReport("Install for %s failed\n", to_install);
                        shiftBackInstallQueue();
                        continue;
                    }
                    
                    woomy_install_index = 0;
                    woomy_processing = true;
                }
                
                woomy_extract_prog = 0;
                woomy_extract_total = 0;
                ezxml_t entries = ezxml_get(woomy_xml, "entries", 0, "entry", -1);
                ezxml_t next_entry = ezxml_idx(entries, woomy_install_index++);
                if(next_entry)
                {
                    OSReport("Installing woomy entry '%s' from '%s'\n", ezxml_attr(next_entry, "name"), ezxml_attr(next_entry, "folder"));
                    woomy_entry_name = ezxml_attr(next_entry, "name");
                    woomy_extract_total = (u32)(strtoul(ezxml_attr(next_entry, "entries"), NULL, 10) & 0xFFFFFFFF);
                    
                    if(!woomy_entry_name)
                        woomy_entry_name = "<no name>";
                    
                    //TODO: tmp to mlc or elsewhere?
                    clear_dir("/vol/external01/tmp/");
                    mkdir("/vol/external01/tmp/", 0x666);
                    
                    woomy_extracting = true;
                    char *temp_tmp_filename = malloc(0x200);
                    
                    for (int i = 0; i < (int)mz_zip_reader_get_num_files(&woomy_archive); i++)
                    {
                        mz_zip_archive_file_stat file_stat;
                        if (!mz_zip_reader_file_stat(&woomy_archive, i, &file_stat))
                        {
                            OSReport("mz_zip_reader_file_stat() failed!\n");
                            continue;
                        }

                        if (!strncmp(file_stat.m_filename, ezxml_attr(next_entry, "folder"), strlen(ezxml_attr(next_entry, "folder"))) && !mz_zip_reader_is_file_a_directory(&woomy_archive, i))
                        {
                            OSReport("Extracting '%s' (Comment: \"%s\", Uncompressed size: %u, Compressed size: %u)\n", file_stat.m_filename, file_stat.m_comment, (uint)file_stat.m_uncomp_size, (uint)file_stat.m_comp_size);
                            
                            snprintf(temp_tmp_filename, 0x200, "/vol/external01/tmp/%s", file_stat.m_filename + strlen(ezxml_attr(next_entry, "folder")));
                            OSReport("%s\n", temp_tmp_filename);
                            mz_zip_reader_extract_file_to_file(&woomy_archive, file_stat.m_filename, temp_tmp_filename, 0);
                            
                            //TODO: Maybe use a callback wrapper to show progress?
                            
                            char *ext = strchr(file_stat.m_filename, '.');
                            if(ext && !strcmp(ext, ".app"))
                                woomy_extract_prog++;
                        }
                    }

                    free(temp_tmp_filename);
                    
                    to_install = malloc(0x200);
                    snprintf(to_install, 0x200, "/vol/app_sd/tmp/");
                }
                else
                {
                    OSReport("Exhausted entries from '%s', advancing install queue.\n", to_install);
                    
                    mz_zip_reader_end(&woomy_archive);
                    ezxml_free(woomy_xml);
                    
                    woomy_archive_name = NULL;
                    woomy_entry_name = NULL;
                    
                    //TODO: tmp to mlc or elsewhere?
                    clear_dir("/vol/external01/tmp/");
                    
                    woomy_extracting = false;
                    woomy_processing = false;
                    shiftBackInstallQueue();
                    continue;
                }
                
                woomy_extracting = false;
            }
            else
            {
                has_icon = false;
            }
            
            if(installDevices[installQueueTarget[0]].deviceID == MCP_INSTALL_TARGET_USB)
            {
                if(MCP_InstallSetTargetUsb(mcp_thread_handle, installDevices[installQueueTarget[0]].deviceNum) < 0)
                {
                    OSReport("Install for %s failed, failed to set install target to USB\n", installQueue[0]);
                    shiftBackInstallQueue();
                    continue;
                }
            }
        
            if(MCP_InstallSetTargetDevice(mcp_thread_handle, installDevices[installQueueTarget[0]].deviceID) >= 0)
            {
                OSReport("Set install target to %s%02u (device ID 0x%x)\n", installDevices[installQueueTarget[0]].deviceName, installDevices[installQueueTarget[0]].deviceNum, installDevices[installQueueTarget[0]].deviceID);
                if(MCP_InstallGetInfo(mcp_thread_handle, to_install, mcp_info_buf) >= 0)
                {
                    if(MCP_InstallTitleAsync(mcp_thread_handle, to_install, mcp_install_buf) >= 0)
                    {
                        installing = true;
                        
                        currentlyInstalling = to_install;
                        if(!woomy_processing)
                            shiftBackInstallQueue();
                        
                        OSReport("Installing %s\n", currentlyInstalling);
                        continue;
                    }
                }
            }
            
            //Install failed for some reason, remove item from queue
            OSReport("Install for %s failed\n", installQueue[0]);
            shiftBackInstallQueue();
        }
    }
    
    free(mcp_info_buf);
    free(mcp_install_buf);
    MCP_Close(mcp_thread_handle);
    return 0;
}

int main(int argc, char **argv)
{
    OSScreenInit();
    OSReport("Screen initted\n");
    
    ProcUIInit(&SaveCallback);
    int initret = fsDevInit();
    currentDirectory = malloc(0x200);
    currentIOSDirectory = malloc(0x200);
    strcpy(currentDirectory, "fs:/vol/external01/");
    strcpy(currentIOSDirectory, "/vol/app_sd/");
    
    OSReport("fsDevInit returned %u\n", initret);
    
    // Allocate MCP buffers
    mcp_handle = MCP_Open();
    mcp_prog_buf = memalign(0x40, 0x24);
    installQueue = calloc(INSTALL_QUEUE_SIZE, sizeof(char*));
    installQueueTarget = calloc(INSTALL_QUEUE_SIZE, sizeof(char));
    
    devicelist = memalign(0x40, sizeof(MCPDeviceList));
    memset(devicelist, 0, sizeof(MCPDeviceList));
    int devret = MCP_FullDeviceList(mcp_handle, &numDevices, devicelist, sizeof(MCPDeviceList));
    
    if(devret == 0x12345678)
    {
        //Fake decaf names
        numDevices = 8;
        strcpy(devicelist->devices[0].name, "usb");
        strcpy(devicelist->devices[1].name, "slccmpt");
        strcpy(devicelist->devices[2].name, "slc");
        strcpy(devicelist->devices[3].name, "sdcard");
        strcpy(devicelist->devices[4].name, "ramdisk");
        strcpy(devicelist->devices[5].name, "drh");
        strcpy(devicelist->devices[6].name, "bt");
        strcpy(devicelist->devices[7].name, "mlc");
    }
    
    //Iterate devices to discover install points
    int numUSB = 1;
    for(int i = 0; i < numDevices; i++)
    {
        if(!strcmp(devicelist->devices[i].name, "usb"))
        {
            if(numUSB == 1)
                selectedInstallTarget = numInstallDevices;
        
            installDevices[numInstallDevices].deviceID = MCP_INSTALL_TARGET_USB;
            installDevices[numInstallDevices].deviceNum = numUSB++;
            installDevices[numInstallDevices].deviceName = devicelist->devices[i].name;
            numInstallDevices++;
            
        }
        else if(!strcmp(devicelist->devices[i].name, "mlc"))
        {
            installDevices[numInstallDevices].deviceID = MCP_INSTALL_TARGET_MLC;
            installDevices[numInstallDevices].deviceNum = 1;
            installDevices[numInstallDevices].deviceName = devicelist->devices[i].name;
            numInstallDevices++;
        }
    }
    
    directoryRead = memalign(0x20, sizeof(struct dirent*)*0x200);
    for(int i = 0; i < 0x200; i++)
    {
        struct dirent *entry = memalign(0x20, sizeof(struct dirent));
        directoryRead[i] = entry;
    }
    
    numEntries = readDirectory(currentDirectory, directoryRead);
    
    OSThread *threadCore2 = OSGetDefaultThread(2);
    OSRunThread(threadCore2, processInstallQueue, 0, NULL);
    
    int error;
	VPADStatus vpad_data;
    while(AppRunning())
    {
        if(!initialized) continue;
    
        VPADRead(0, &vpad_data, 1, &error);
        
        lastButtonState = buttonState;
        buttonState = false;
        int listLimit = screenSwap ? 31 : 22;
        
        if(vpad_data.trigger & VPAD_BUTTON_DOWN)
        {
            if(selectedFile < numEntries-1)
                selectedFile++;
                
            if(selectedFile >= scrollPos+listLimit)
                scrollPos++;
        }
        else if(vpad_data.trigger & VPAD_BUTTON_UP)
        {
            if(selectedFile > 0)
                selectedFile--;
                
            if(selectedFile < scrollPos)
                scrollPos--;
        }
        else if(vpad_data.trigger & VPAD_BUTTON_RIGHT)
        {
            if(selectedFile < numEntries-1)
                selectedFile = MIN(selectedFile + 5, numEntries-1);
                
            if(selectedFile >= scrollPos+listLimit)
                scrollPos = selectedFile - listLimit + 1;
        }
        else if(vpad_data.trigger & VPAD_BUTTON_LEFT)
        {
            if(selectedFile > 0)
                selectedFile = MAX(selectedFile - 5, 0);
                
            if(selectedFile < scrollPos)
                scrollPos = selectedFile;
        }
        else if(vpad_data.trigger & VPAD_BUTTON_A)
        {
            if(directoryRead[selectedFile]->d_type & DT_DIR)
            {
                dirLevel++;
                
                strcat(currentDirectory, directoryRead[selectedFile]->d_name);
                strcat(currentDirectory, "/");
                strcat(currentIOSDirectory, directoryRead[selectedFile]->d_name);
                strcat(currentIOSDirectory, "/");
                int entries = readDirectory(currentDirectory, directoryRead);
                if(!entries)
                {
                    moveUpDirectory();
                    numEntries = readDirectory(currentDirectory, directoryRead);
                }
                else
                    numEntries = entries;
            }
            else
            {
                //Add woomy to install queue
                char *tempstr = malloc(strlen(currentDirectory)+strlen(directoryRead[selectedFile]->d_name)+1);
                strcpy(tempstr, currentDirectory);
                strcat(tempstr, directoryRead[selectedFile]->d_name);
                addToInstallQueue(tempstr);
                free(tempstr);
            }
        }
        else if(vpad_data.trigger & VPAD_BUTTON_B)
        {
            if(dirLevel != 0)
            {
                dirLevel--;
                
                moveUpDirectory();
                numEntries = readDirectory(currentDirectory, directoryRead);
            }
        }
        else if(vpad_data.trigger & VPAD_BUTTON_Y)
        {
            if(directoryRead[selectedFile]->d_type & DT_DIR)
            {
                char *toInstallDir = malloc(strlen(currentIOSDirectory) + strlen(directoryRead[selectedFile]->d_name) + 2);
                strcpy(toInstallDir, currentIOSDirectory);
                strcat(toInstallDir, directoryRead[selectedFile]->d_name);
                strcat(toInstallDir, "/");
                addToInstallQueue(toInstallDir);
                free(toInstallDir);
            }
            else
            {
                addToInstallQueue(currentIOSDirectory);
            }
        }
        else if(vpad_data.trigger & VPAD_BUTTON_X)
        {
            if(installing)
            {
                MCP_InstallTitleAbort(mcp_handle);
            }
        }
        else if(vpad_data.trigger & VPAD_BUTTON_MINUS)
        {
            screenSwap = !screenSwap;
            listLimit = screenSwap ? 31 : 22;
            scrollPos = MIN(numEntries < listLimit ? 0 : numEntries - listLimit, selectedFile);
        }
        else if(vpad_data.trigger & VPAD_BUTTON_PLUS)
        {
            selectedInstallTarget = (selectedInstallTarget + 1) % numInstallDevices;
        }
    
        setActiveScreen(screenSwap ? SCREEN_BOTTOM : SCREEN_TOP);
        fillScreen(0,0,0,0);
        drawBorder(9, 0xC9, 0x34, 0x57, 0);
        
        centerStringf(20,"Woom\xefnstaller");
        drawString(20, 40, currentDirectory);
        drawStringf(20, 60, "Current Install Target: \x80\xFF\xAA\xAA%s%02u", installDevices[selectedInstallTarget].deviceName, installDevices[selectedInstallTarget].deviceNum);
        
        if(installing)
        {
            int ret = MCP_InstallGetProgress(mcp_handle, mcp_prog_buf);
            
            //Fake info for decaf
            if(ret == 0x12345678)
            {
                mcp_prog_buf->inProgress = true;
                mcp_prog_buf->sizeProgress = 5;
                mcp_prog_buf->sizeTotal = 11;
                mcp_prog_buf->contentsProgress = 17;
                mcp_prog_buf->contentsTotal = 30;
            }
            
            //TODO: Show woomy metadata instead of TID?
            drawRectThickness(30, 90, getScreenWidth() - 30, 260+(has_icon?110:0), 2, 255,255,255,0);
            
            if(!screenSwap)
                drawStringf(42, 100, "Installing \x80\xFF\xAA\xAA%016llX\x80\xFF\xFF\xFF from \x80\xFF\xAA\xAA%s", mcp_prog_buf->tid, currentlyInstalling);
            else
                drawStringf(42, 100, "Installing \x80\xFF\xAA\xAA%016llX", mcp_prog_buf->tid);
                
            drawStringf(42, 120, "%llu of %llu bytes written", mcp_prog_buf->sizeProgress, mcp_prog_buf->sizeTotal);
            drawStringf(42, 140, "Installing content %u out of %u", mcp_prog_buf->contentsProgress, mcp_prog_buf->contentsTotal);
            
            drawRectThickness(40, 170+(has_icon?110:0), getScreenWidth() - 40, 205+(has_icon?110:0), 2, 128,128,128,0);
            if(mcp_prog_buf->sizeProgress > 0)
            {
                float installPercent = ((float)mcp_prog_buf->sizeProgress / (float)mcp_prog_buf->sizeTotal)*100.0f;
                float installPartPercent = ((float)mcp_prog_buf->sizeProgress / (float)mcp_prog_buf->sizeTotal);
                drawFillRect(40+4, 170+4+(has_icon?110:0), MAX(40+4, ((getScreenWidth() - 40)-4)*installPartPercent), 205-4+(has_icon?110:0), 255, 170, 170, 0);
                centerStringf(215+(has_icon?110:0), "%5.1f%% complete", installPercent);
            }
            
            if(has_icon)
            {
                drawTGA(getScreenWidth() - 60 - 128, 130, icon_mem);
            }
        }
        else if(woomy_extracting)
        {
            drawRectThickness(30, 90, getScreenWidth() - 30, 260+(has_icon?110:0), 2, 255,255,255,0);
            
            if(!screenSwap)
                drawStringf(42, 100,                        "Preparing to install \x80\xFF\xAA\xAA%s\x80\xFF\xFF\xFF from \x80\xFF\xAA\xAA%s\x80\xFF\xFF\xFF", woomy_entry_name, woomy_archive_name);
            else
                drawStringf(42, 100,                        "Preparing to install \x80\xFF\xAA\xAA%s\x80\xFF\xFF\xFF", woomy_entry_name);
                
            drawStringf(42, 120, "Unpacking contents %u of %u", woomy_extract_prog, woomy_extract_total);
            drawRectThickness(40, 170+(has_icon?110:0), getScreenWidth() - 40, 205+(has_icon?110:0), 2, 128,128,128,0);
            //TODO: Maybe show extraction progress?
            /*if(mcp_prog_buf->sizeProgress > 0)
            {
                float installPercent = ((float)mcp_prog_buf->sizeProgress / (float)mcp_prog_buf->sizeTotal)*100.0f;
                float installPartPercent = ((float)mcp_prog_buf->sizeProgress / (float)mcp_prog_buf->sizeTotal);
                drawFillRect(40+4, 150+4+(has_icon?110:0), MAX(40+4, ((getScreenWidth() - 40)-4)*installPartPercent), 185-4+(has_icon?110:0), 255, 170, 170, 0);
                centerStringf(195+(has_icon?110:0), "%5.1f%% complete", installPercent);
            }*/
            
            if(has_icon)
            {
                drawTGA(getScreenWidth() - 60 - 128, 130, icon_mem);
            }
        }
        else
        {   
            if(directoryRead[selectedFile]->d_type == DT_DIR)
                drawStringfColor(20,100,255,170,170,0," %s/", directoryRead[selectedFile]->d_name);
            else
                drawStringfColor(20,100,255,170,170,0," %s", directoryRead[selectedFile]->d_name);
        }
        
        if(installQueue[0] != NULL)
        {
            int yPos = 300+(has_icon?110:0);
            int yLimit = screenSwap ? getScreenHeight()-40 : getScreenHeight()-100;
            drawStringf(20, yPos, "Current install queue:");
            for(int i = 0; i < INSTALL_QUEUE_SIZE; i++)
            {
                if(installQueue[i] == NULL)
                    break;
                
                yPos += 20;
                drawStringfColor(30,yPos,255,170,170,0, (yPos >= yLimit && installQueue[i+1] != NULL) ? "%s" : "%-41s -> %s%02u",(yPos >= yLimit && installQueue[i+1] != NULL) ? "..." : installQueue[i], installDevices[installQueueTarget[i]].deviceName, installDevices[installQueueTarget[i]].deviceNum);
                if(yPos >= yLimit)
                    break;
            }
        }
        
        //Control usage
        if(!screenSwap)
        {
            drawStringf(20,getScreenHeight()-50,                    "A:                     B:              Y:                   X: ");
            drawStringfColor(20,getScreenHeight()-50,255,170,170,0, "                          Exit Folder     Add FST to queue     Cancel Install");
            drawStringfColor(20,getScreenHeight()-60,255,170,170,0, "   Enter Folder");
            drawStringfColor(20,getScreenHeight()-40,255,170,170,0, "   Add Woomy to queue");
        }
        
        //Swap screen button
        if(screenSwap)
        {
            VPADTouchData tpCalib;
            VPADGetTPCalibratedPoint(0, &tpCalib, &vpad_data.tpNormal);
            int tpxpos = (int)(((float)tpCalib.x / 1280.0f) * (float)getScreenWidth());
			int tpypos = (int)(((float)tpCalib.y / 720.0f) * (float)getScreenHeight());

            if(vpad_data.tpNormal.touched && tpxpos > getScreenWidth()-160 && tpxpos < getScreenWidth() - 20 && tpypos > 20 && tpypos < 80)
                buttonState = true;
        
            u8 color = buttonState ? 60 : 20;
            drawFillRect(getScreenWidth()-160, 20, getScreenWidth() - 20, 80, color,color,color,0);
            drawRectThickness(getScreenWidth()-160, 20, getScreenWidth() - 20, 80, 2, 128,128,128,0);
            drawStringf(getScreenWidth()-140, 30," Swap");
            drawStringf(getScreenWidth()-160, 50," Screens");
        }
        
        flipBuffers();
        
        setActiveScreen(screenSwap ? SCREEN_TOP : SCREEN_BOTTOM);
        fillScreen(0,0,0,0);
        drawBorder(9, 4, 0x81, 0x88, 0);
        
        int ypos = 20;
        for(int i = scrollPos; i < MIN(scrollPos+listLimit, numEntries); i++)
        {
            if(directoryRead[i] == NULL)
                continue;
        
            char buf[256];
            drawStringfColor(20, ypos, 0, 255, 255, 0, " %s", selectedFile == i ? ">" : " ");
            if(directoryRead[i]->d_type == DT_DIR)
                drawStringfColor(20, ypos, 255, 255, 255, 0, "   %s/", directoryRead[i]->d_name);
            else
                drawStringfColor(20, ypos, 150, 255, 255, 0, "   %s", directoryRead[i]->d_name);
            
            ypos += 20;
        }
        
        //Swap screen button
        if(!screenSwap)
        {
            VPADTouchData tpCalib;
            VPADGetTPCalibratedPoint(0, &tpCalib, &vpad_data.tpNormal);
            int tpxpos = (int)(((float)tpCalib.x / 1280.0f) * (float)getScreenWidth());
			int tpypos = (int)(((float)tpCalib.y / 720.0f) * (float)getScreenHeight());

            if(vpad_data.tpNormal.touched && tpxpos > getScreenWidth()-160 && tpxpos < getScreenWidth() - 20 && tpypos > 20 && tpypos < 80)
                buttonState = true;
        
            u8 color = buttonState ? 60 : 20;
            drawFillRect(getScreenWidth()-160, 20, getScreenWidth() - 20, 80, color,color,color,0);
            drawRectThickness(getScreenWidth()-160, 20, getScreenWidth() - 20, 80, 2, 128,128,128,0);
            drawStringf(getScreenWidth()-140, 30," Swap");
            drawStringf(getScreenWidth()-160, 50," Screens");
        }
        
        //Control usage
        if(screenSwap)
        {
            drawStringf(20,getScreenHeight()-50,                    "A:                     B:              Y:                   X: ");
            drawStringfColor(20,getScreenHeight()-50,150, 255, 255, 0, "                          Exit Folder     Add FST to queue     Cancel Install");
            drawStringfColor(20,getScreenHeight()-60,150, 255, 255, 0, "   Enter Folder");
            drawStringfColor(20,getScreenHeight()-40,150, 255, 255, 0, "   Add Woomy to queue");
        }
        
        if(buttonState != lastButtonState && !lastButtonState)
        {
            screenSwap = !screenSwap;
            listLimit = screenSwap ? 31 : 22;
            scrollPos = MIN(numEntries < listLimit ? 0 : numEntries - listLimit, selectedFile);
        }
    
        flipBuffers();
    }
    
    free(mcp_prog_buf);
    free(installQueue);
    free(installQueueTarget);
    MCP_Close(mcp_handle);
    return 0;
}
