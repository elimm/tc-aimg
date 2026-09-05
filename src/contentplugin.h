#ifndef CONTENTPLUGIN_H
#define CONTENTPLUGIN_H

#include <windows.h>

// A deliberate subset of the official Total Commander WDX SDK's
// src/contplug.h (version 2.12), in this plugin's formatting style. The SDK
// -- contplug.h and the docs/*.htm pages cited here and in aimg.cpp -- is
// not vendored; download it from ghisler/WDX-SDK to follow a reference.
//
// These values cross the DLL boundary as raw ints against Total Commander's
// own independently compiled copy, so drift here is a live ABI defect, not
// an internal detail. Diff against the real header before adding anything;
// never invent a constant or prototype the published SDK lacks.

#ifdef __cplusplus
extern "C" {
#endif

/* Field types for Total Commander WDX plugins */
#define ft_nomorefields 0
#define ft_numeric_32 1
#define ft_numeric_64 2
#define ft_numeric_floating 3
#define ft_date 4
#define ft_time 5
#define ft_boolean 6
#define ft_multiplechoice 7
#define ft_string 8
#define ft_fulltext 9
#define ft_datetime 10
#define ft_stringw 11
#define ft_fulltextw 12
#define ft_comparecontent 100

/* Return values for ContentGetValue / ContentGetValueW */
#define ft_nosuchfield -1   /* error, invalid field number given */
#define ft_fileerror -2     /* file i/o error */
#define ft_fieldempty -3    /* field valid, but empty */
#define ft_ondemand -4      /* field retrieved only when user presses <SPACEBAR> */
#define ft_notsupported -5  /* function not supported */
#define ft_setcancel -6     /* user clicked cancel in field editor */
#define ft_delayed 0        /* field takes a long time to extract -> try again in background */

/* Flags returned from ContentGetSupportedFieldFlags(), an optional export
 * this plugin does not implement. Reference only -- they must never be OR'd
 * into ContentGetSupportedField's own return value.
 */
#define contflags_edit 1
#define contflags_substsize 2
#define contflags_substdatetime 4
#define contflags_substdate 6
#define contflags_substtime 8
#define contflags_substattributes 10
#define contflags_substattributestr 12
#define contflags_passthrough_size_float 14
#define contflags_substmask 14
#define contflags_fieldedit 16
#define contflags_fieldsearch 32
#define contflags_searchpageonly 64

/* State values for ContentSendStateInformation(W) */
#define contst_readnewdir 1
#define contst_refreshpressed 2
#define contst_showhint 4

/* Flags parameter of ContentGetValue(W) */
#define CONTENT_DELAYIFSLOW 1
#define CONTENT_PASSTHROUGH 2

/* Default parameter struct */
typedef struct {
    int size;
    DWORD PluginInterfaceVersionLow;
    DWORD PluginInterfaceVersionHi;
    char DefaultIniName[MAX_PATH];
} ContentDefaultParamStruct;

/* Per docs/unicode_support.htm, ContentGetDetectString,
 * ContentGetSupportedField and ContentSetDefaultParams have no Unicode form
 * in the real ABI and must stay ANSI.
 */
int __stdcall ContentGetSupportedField(int FieldIndex, char* FieldName, char* Units, int maxlen);
int __stdcall ContentGetValue(char* FileName, int FieldIndex, int UnitIndex, void* FieldValue, int maxlen, int flags);
int __stdcall ContentGetValueW(WCHAR* FileName, int FieldIndex, int UnitIndex, void* FieldValue, int maxlen, int flags);
void __stdcall ContentSetDefaultParams(ContentDefaultParamStruct* dps);
int __stdcall ContentGetDetectString(char* DetectString, int maxlen);
void __stdcall ContentSendStateInformation(int state, char* path);
void __stdcall ContentSendStateInformationW(int state, WCHAR* path);
void __stdcall ContentStopGetValue(char* FileName);
void __stdcall ContentStopGetValueW(WCHAR* FileName);

#ifdef __cplusplus
}
#endif

#endif /* CONTENTPLUGIN_H */
