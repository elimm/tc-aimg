#ifndef CONTENTPLUGIN_H
#define CONTENTPLUGIN_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Field types for Total Commander WDX plugins */
#define ft_numeric_32 1
#define ft_numeric_64 2
#define ft_float 3
#define ft_date 4
#define ft_time 5
#define ft_boolean 6
#define ft_multiplechoice 7
#define ft_string 8
#define ft_fulltext 9
#define ft_datetime 10
#define ft_stringw 11
#define ft_fulltextw 12
#define ft_numeric_floating 13
#define ft_time_seconds 14

/* Flags for ContentGetValue / ContentGetValueW */
#define ft_setdir -1
#define ft_delay 0
#define ft_ondemand 1

/* Flags for ContentGetSupportedField */
#define contflags_edit 1
#define contflags_substsize 2
#define contflags_substdatetime 4
#define contflags_substdate 8
#define contflags_substtime 16
#define contflags_substattributes 32
#define contflags_substattribs 32
#define contflags_fieldname 64
#define contflags_fieldhint 128
#define contflags_passthrough_exif 256

/* Return values for ContentGetValue / ContentGetValueW */
#define ft_fieldempty 0
#define ft_fileerror -1
#define ft_nosuchfield -2
#define ft_notsupported -4
#define ft_setdir_delete -5

/* Default parameter struct */
typedef struct {
    int size;
    DWORD PluginInterfaceVersionLow;
    DWORD PluginInterfaceVersionHi;
    char DefaultIniName[MAX_PATH];
} ContentDefaultParamStruct;

/* Unicode parameter struct */
typedef struct {
    int size;
    DWORD PluginInterfaceVersionLow;
    DWORD PluginInterfaceVersionHi;
    WCHAR DefaultIniName[MAX_PATH];
} ContentDefaultParamStructW;

/* Exported plugin API function prototypes */
int __stdcall ContentGetSupportedField(int FieldIndex, char* FieldName, char* Units, int maxlen);
int __stdcall ContentGetSupportedFieldW(int FieldIndex, WCHAR* FieldName, WCHAR* Units, int maxlen);
int __stdcall ContentGetValue(char* FileName, int FieldIndex, int UnitIndex, void* FieldValue, int maxlen, int flags);
int __stdcall ContentGetValueW(WCHAR* FileName, int FieldIndex, int UnitIndex, void* FieldValue, int maxlen, int flags);
void __stdcall ContentSetDefaultParams(ContentDefaultParamStruct* dps);
void __stdcall ContentSetDefaultParamsW(ContentDefaultParamStructW* dps);
int __stdcall ContentGetDetectString(char* DetectString, int maxlen);
void __stdcall ContentSendStateInformation(int state, char* path);
void __stdcall ContentSendStateInformationW(int state, WCHAR* path);

#ifdef __cplusplus
}
#endif

#endif /* CONTENTPLUGIN_H */
