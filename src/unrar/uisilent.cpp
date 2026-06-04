/*
 * The silent UnRAR UI normally discards all messages. BFpilot keeps the last
 * error message so the HTTP layer can report useful extraction failures.
 */
#ifdef BFPILOT_UNRAR_STREAM
static thread_local char bfpilot_unrar_last_msg[1024];
extern "C" bool bfpilot_unrar_cancelled(void);

extern "C" void bfpilot_unrar_ui_reset(void)
{
  bfpilot_unrar_last_msg[0]=0;
}

extern "C" const char *bfpilot_unrar_ui_last_message(void)
{
  return bfpilot_unrar_last_msg;
}

static const char *bfpilot_unrar_ui_code_name(UIMESSAGE_CODE Code)
{
  switch (Code)
  {
    case UIERROR_CHECKSUM: return "checksum failed";
    case UIERROR_CHECKSUMENC: return "encrypted checksum failed";
    case UIERROR_CHECKSUMPACKED: return "packed data checksum failed";
    case UIERROR_BADPSW: return "bad password";
    case UIWAIT_BADPSW: return "bad password";
    case UIERROR_MEMORY: return "out of memory";
    case UIERROR_FILEOPEN: return "cannot open file";
    case UIERROR_FILECREATE: return "cannot create file";
    case UIERROR_FILECLOSE: return "cannot close file";
    case UIERROR_FILESEEK: return "cannot seek file";
    case UIERROR_FILEREAD: return "cannot read file";
    case UIERROR_FILEWRITE: return "cannot write file";
    case UIERROR_FILERENAME: return "cannot rename file";
    case UIERROR_DIRCREATE: return "cannot create directory";
    case UIERROR_SLINKCREATE: return "cannot create symlink";
    case UIERROR_HLINKCREATE: return "cannot create hardlink";
    case UIERROR_ARCBROKEN: return "archive is broken";
    case UIERROR_HEADERBROKEN: return "archive header is broken";
    case UIERROR_MHEADERBROKEN: return "main header is broken";
    case UIERROR_FHEADERBROKEN: return "file header is broken";
    case UIERROR_SUBHEADERBROKEN: return "subheader is broken";
    case UIERROR_SUBHEADERUNKNOWN: return "unknown subheader";
    case UIERROR_SUBHEADERDATABROKEN: return "subheader data is broken";
    case UIERROR_UNKNOWNMETHOD: return "unknown compression method";
    case UIERROR_UNKNOWNENCMETHOD: return "unknown encryption method";
    case UIERROR_EXTRDICTOUTMEM: return "dictionary limit exceeded";
    case UIERROR_UNEXPEOF: return "unexpected end of archive";
    case UIERROR_BADARCHIVE: return "bad archive";
    case UIERROR_INVALIDNAME: return "invalid filename";
    case UIERROR_NOFILESTOEXTRACT: return "no files to extract";
    case UIERROR_MISSINGVOL: return "missing volume";
    case UIERROR_NEEDPREVVOL: return "previous volume is required";
    case UIERROR_NOTFIRSTVOLUME: return "first volume is required";
    case UIERROR_NOTSUPPORTED: return "archive feature is not supported";
    case UIERROR_ENCRNOTSUPPORTED: return "encryption is not supported";
    case UIERROR_PATHTOOLONG: return "path is too long";
    case UIERROR_SKIPUNSAFELINK: return "unsafe link skipped";
    case UIERROR_OPFAILED: return "operation failed";
    default: return "unrar error";
  }
}

static void bfpilot_unrar_append(char **out, size_t *left, const char *text)
{
  if (*left == 0 || text == nullptr || text[0] == 0)
    return;

  int n=snprintf(*out,*left,"%s",text);
  if (n < 0)
    return;
  size_t used=(size_t)n >= *left ? *left - 1 : (size_t)n;
  *out+=used;
  *left-=used;
}

static void bfpilot_unrar_append_wide(char **out, size_t *left,
                                      const wchar *text)
{
  if (text == nullptr || text[0] == 0 || *left == 0)
    return;

  char tmp[256];
  WideToUtf(text,tmp,sizeof(tmp));
  bfpilot_unrar_append(out,left,tmp);
}

static void bfpilot_unrar_store_ui_msg(UIMESSAGE_CODE Code,
                                       const wchar **Str, uint StrSize,
                                       uint *Num, uint NumSize)
{
  if ((Code >= UIMSG_FIRST && Code < UIWAIT_FIRST) || Code >= UIEVENT_FIRST)
    return;

  char *out=bfpilot_unrar_last_msg;
  size_t left=sizeof(bfpilot_unrar_last_msg);
  bfpilot_unrar_append(&out,&left,bfpilot_unrar_ui_code_name(Code));

  bool wrote_string=false;
  for (uint i=0;i<StrSize && i<3;i++)
  {
    if (Str[i] == nullptr || Str[i][0] == 0)
      continue;
    bfpilot_unrar_append(&out,&left,wrote_string ? " | " : " [");
    bfpilot_unrar_append_wide(&out,&left,Str[i]);
    wrote_string=true;
  }
  if (wrote_string)
    bfpilot_unrar_append(&out,&left,"]");

  if (NumSize > 0)
  {
    char tmp[64];
    snprintf(tmp,sizeof(tmp)," (%u)",Num[0]);
    bfpilot_unrar_append(&out,&left,tmp);
  }
}
#endif

// Purely user interface function. Gets and returns user input.
UIASKREP_RESULT uiAskReplace(std::wstring &Name,int64 FileSize,RarTime *FileTime,uint Flags)
{
  return UIASKREP_R_REPLACE;
}




void uiStartArchiveExtract(bool Extract,const std::wstring &ArcName)
{
}


bool uiStartFileExtract(const std::wstring &FileName,bool Extract,bool Test,bool Skip)
{
  return true;
}


void uiExtractProgress(int64 CurFileSize,int64 TotalFileSize,int64 CurSize,int64 TotalSize)
{
}


void uiProcessProgress(const char *Command,int64 CurSize,int64 TotalSize)
{
}


void uiMsgStore::Msg()
{
#ifdef BFPILOT_UNRAR_STREAM
  bfpilot_unrar_store_ui_msg(Code,Str,StrSize,Num,NumSize);
#endif
}


bool uiGetPassword(UIPASSWORD_TYPE Type,const std::wstring &FileName,
                   SecPassword *Password,CheckPassword *CheckPwd)
{
#ifdef BFPILOT_UNRAR_STREAM
  char *out=bfpilot_unrar_last_msg;
  size_t left=sizeof(bfpilot_unrar_last_msg);
  bfpilot_unrar_append(&out,&left,"password required");
  if (!FileName.empty())
  {
    bfpilot_unrar_append(&out,&left," [");
    bfpilot_unrar_append_wide(&out,&left,FileName.c_str());
    bfpilot_unrar_append(&out,&left,"]");
  }
#endif
  return false;
}


bool uiIsGlobalPasswordSet()
{
  return false;
}


void uiAlarm(UIALARM_TYPE Type)
{
}


bool uiIsAborted()
{
#ifdef BFPILOT_UNRAR_STREAM
  return bfpilot_unrar_cancelled();
#else
  return false;
#endif
}


void uiGiveTick()
{
}


bool uiDictLimit(CommandData *Cmd,const std::wstring &FileName,uint64 DictSize,uint64 MaxDictSize)
{
#ifdef BFPILOT_UNRAR_STREAM
  char *out=bfpilot_unrar_last_msg;
  size_t left=sizeof(bfpilot_unrar_last_msg);
  bfpilot_unrar_append(&out,&left,"dictionary limit exceeded [");
  bfpilot_unrar_append_wide(&out,&left,FileName.c_str());
  char tmp[96];
  snprintf(tmp,sizeof(tmp),"] need=%llu max=%llu",
           (unsigned long long)DictSize,
           (unsigned long long)MaxDictSize);
  bfpilot_unrar_append(&out,&left,tmp);
#endif
#ifdef RARDLL
  if (Cmd->Callback!=nullptr &&
      Cmd->Callback(UCM_LARGEDICT,Cmd->UserData,(LPARAM)(DictSize/1024),(LPARAM)(MaxDictSize/1024))==1)
    return true; // Continue extracting if unrar.dll callback permits it.
#endif
  return false; // Stop extracting.
}


#ifndef SFX_MODULE
const wchar *uiGetMonthName(uint Month)
{
  return L"";
}
#endif


void uiEolAfterMsg()
{
}
