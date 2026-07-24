/*
 * policy_engine.c - kernel-side policy engine
 */

#include "policy_engine.h"
#include <ntstrsafe.h>
#include "../filter/process_tracker.h"
#include "../common/shared_events.h"
#include "../core/protect.h"

static POLICY_ENGINE_STATE gPolicyState;
static const WCHAR gDefaultQuarantineRoot[] =
    L"\\??\\C:\\ProgramData\\PersonalSafer\\Quarantine";
#define PS_POLICY_JSON_PATH_CHARS 128
#define PS_MAX_JSON_PREDICATE_GROUP 4
#define PS_MAX_JSON_PREDICATE_COND 4

typedef enum _JSON_PREDICATE_OP {
    JsonPredicateOpEq = 0,
    JsonPredicateOpNe,
    JsonPredicateOpGt,
    JsonPredicateOpGe,
    JsonPredicateOpLt,
    JsonPredicateOpLe
} JSON_PREDICATE_OP;

static
BOOLEAN
EndsWithNoCase(
    _In_ PCWSTR Name,
    _In_ PCWSTR Suffix
    )
{
    SIZE_T nameLen;
    SIZE_T suffixLen;

    if (Name == NULL || Suffix == NULL) return FALSE;

    nameLen = wcslen(Name);
    suffixLen = wcslen(Suffix);
    if (suffixLen == 0 || suffixLen > nameLen) return FALSE;

    return _wcsnicmp(Name + (nameLen - suffixLen), Suffix, suffixLen) == 0;
}

static
BOOLEAN
ContainsNoCase(
    _In_ PCWSTR Text,
    _In_ PCWSTR Needle
    )
{
    SIZE_T textLen;
    SIZE_T needleLen;
    SIZE_T i;

    if (Text == NULL || Needle == NULL) return FALSE;

    textLen = wcslen(Text);
    needleLen = wcslen(Needle);
    if (needleLen == 0 || needleLen > textLen) return FALSE;

    for (i = 0; i + needleLen <= textLen; i++) {
        if (_wcsnicmp(Text + i, Needle, needleLen) == 0) {
            return TRUE;
        }
    }

    return FALSE;
}

static
PCWSTR
FindHostInUrl(
    _In_ PCWSTR Url
    )
{
    PCWSTR scheme;

    if (Url == NULL) {
        return NULL;
    }

    scheme = wcsstr(Url, L"://");
    return scheme != NULL ? scheme + 3 : Url;
}

static
BOOLEAN
DomainMatchesNoCase(
    _In_ PCWSTR HostOrDomain,
    _In_ PCWSTR RuleDomain
    )
{
    WCHAR hostBuf[256];
    SIZE_T i = 0;
    SIZE_T hostLen;
    SIZE_T ruleLen;
    PCWSTR host;

    if (HostOrDomain == NULL || RuleDomain == NULL) return FALSE;

    host = FindHostInUrl(HostOrDomain);
    while (host[i] != L'\0' && host[i] != L'/' && host[i] != L':' &&
           host[i] != L'?' && host[i] != L'#' && i < ARRAYSIZE(hostBuf) - 1) {
        hostBuf[i] = host[i];
        i++;
    }
    hostBuf[i] = L'\0';

    hostLen = wcslen(hostBuf);
    ruleLen = wcslen(RuleDomain);
    if (ruleLen == 0 || ruleLen > hostLen) return FALSE;

    if (_wcsicmp(hostBuf, RuleDomain) == 0) {
        return TRUE;
    }

    return hostLen > ruleLen &&
        hostBuf[hostLen - ruleLen - 1] == L'.' &&
        _wcsnicmp(hostBuf + (hostLen - ruleLen), RuleDomain, ruleLen) == 0;
}

static
BOOLEAN
FtpCommandMatchesNoCase(
    _In_ PCWSTR Content,
    _In_ PCWSTR RuleCommand
    )
{
    WCHAR command[PS_FTP_CMD_LEN];
    SIZE_T i = 0;
    PCWSTR start = Content;

    if (Content == NULL || RuleCommand == NULL) return FALSE;

    if (_wcsnicmp(Content, L"FTP ", 4) == 0 && wcsstr(Content, L"command=\"") != NULL) {
        start = wcsstr(Content, L"command=\"") + 9;
    } else if (_wcsnicmp(Content, L"CMD ", 4) == 0) {
        start = Content + 4;
    } else if (_wcsnicmp(Content, L"DATA ", 5) == 0) {
        start = Content + 5;
        while (*start != L'\0' && *start != L' ') start++;
        while (*start == L' ') start++;
    }

    while (start[i] != L'\0' && start[i] != L' ' && start[i] != L'"' &&
           i < ARRAYSIZE(command) - 1) {
        command[i] = start[i];
        i++;
    }
    command[i] = L'\0';

    return i > 0 && _wcsicmp(command, RuleCommand) == 0;
}

static
BOOLEAN
FtpSpanContainsNoCase(
    _In_reads_(SpanLength) PCWSTR Span,
    _In_ SIZE_T SpanLength,
    _In_z_ PCWSTR Pattern
    )
{
    SIZE_T patternLength;
    SIZE_T i;

    if (Span == NULL || Pattern == NULL) return FALSE;
    patternLength = wcslen(Pattern);
    if (patternLength == 0 || patternLength > SpanLength) return FALSE;
    for (i = 0; i + patternLength <= SpanLength; i++) {
        if (_wcsnicmp(Span + i, Pattern, patternLength) == 0) return TRUE;
    }
    return FALSE;
}

static
BOOLEAN
FtpQuotedFieldContainsNoCase(
    _In_z_ PCWSTR Content,
    _In_z_ PCWSTR FieldPrefix,
    _In_z_ PCWSTR Pattern
    )
{
    PCWSTR start;
    PCWSTR end;

    start = wcsstr(Content, FieldPrefix);
    if (start == NULL) return FALSE;
    start += wcslen(FieldPrefix);
    end = wcschr(start, L'"');
    if (end == NULL) end = start + wcslen(start);
    return FtpSpanContainsNoCase(start, (SIZE_T)(end - start), Pattern);
}

static
BOOLEAN
FtpPathMatchesNoCase(
    _In_z_ PCWSTR Content,
    _In_z_ PCWSTR Pattern
    )
{
    PCWSTR start;
    PCWSTR end;

    if (Content == NULL || Pattern == NULL || Pattern[0] == L'\0') return FALSE;
    if (_wcsnicmp(Content, L"FTP DIR_ENTRY ", 14) == 0) {
        return FtpQuotedFieldContainsNoCase(Content, L"name=\"", Pattern);
    }
    if (_wcsnicmp(Content, L"FTP DATA_CONTENT ", 17) == 0) {
        start = wcsstr(Content, L"command=\"");
        if (start == NULL) return FALSE;
        start += 9;
        while (*start != L'\0' && *start != L' ' && *start != L'"') start++;
        while (*start == L' ') start++;
        end = wcschr(start, L'"');
        if (end == NULL) end = start + wcslen(start);
        return FtpSpanContainsNoCase(start, (SIZE_T)(end - start), Pattern);
    }

    start = Content;
    if (_wcsnicmp(start, L"CMD ", 4) == 0) start += 4;
    while (*start != L'\0' && *start != L' ') start++;
    while (*start == L' ') start++;
    return *start != L'\0' && ContainsNoCase(start, Pattern);
}

static
BOOLEAN
FtpContentMatchesNoCase(
    _In_z_ PCWSTR Content,
    _In_z_ PCWSTR Pattern
    )
{
    if (_wcsnicmp(Content, L"FTP DATA_CONTENT ", 17) != 0) return FALSE;
    return FtpQuotedFieldContainsNoCase(Content, L"preview=\"", Pattern);
}

static
WCHAR
FoldNoCaseChar(
    _In_ WCHAR Ch
    )
{
    if (Ch >= L'A' && Ch <= L'Z') {
        return (WCHAR)(Ch - L'A' + L'a');
    }
    return Ch;
}

static
VOID
NormalizeJsonPathRule(
    _In_z_ PCWSTR Rule,
    _Out_writes_(OutChars) PWCHAR Out,
    _In_ SIZE_T OutChars
    )
{
    SIZE_T inIndex = 0;
    SIZE_T outIndex = 0;

    if (OutChars == 0) {
        return;
    }

    while (Rule != NULL && Rule[inIndex] != L'\0' && outIndex + 1 < OutChars) {
        if (Rule[inIndex] == L'[' && Rule[inIndex + 1] == L']') {
            if (outIndex + 3 >= OutChars) {
                break;
            }
            Out[outIndex++] = L'[';
            Out[outIndex++] = L'*';
            Out[outIndex++] = L']';
            inIndex += 2;
            continue;
        }
        Out[outIndex++] = Rule[inIndex++];
    }

    Out[outIndex] = L'\0';
}

static
BOOLEAN
GlobMatchNoCase(
    _In_z_ PCWSTR Text,
    _In_z_ PCWSTR Pattern
    )
{
    PCWSTR starPattern = NULL;
    PCWSTR starText = NULL;

    if (Text == NULL || Pattern == NULL) {
        return FALSE;
    }

    while (*Text != L'\0') {
        if (*Pattern == L'*') {
            starPattern = ++Pattern;
            starText = Text;
            continue;
        }

        if (*Pattern != L'\0' &&
            FoldNoCaseChar(*Pattern) == FoldNoCaseChar(*Text)) {
            Pattern++;
            Text++;
            continue;
        }

        if (starPattern != NULL) {
            Pattern = starPattern;
            Text = ++starText;
            continue;
        }

        return FALSE;
    }

    while (*Pattern == L'*') {
        Pattern++;
    }

    return *Pattern == L'\0';
}

static
BOOLEAN
JsonPathRuleMatchesNoCase(
    _In_z_ PCWSTR Path,
    _In_z_ PCWSTR Rule
    )
{
    WCHAR normalizedRule[PS_POLICY_JSON_PATH_CHARS];

    if (Path == NULL || Rule == NULL || Rule[0] == L'\0') {
        return FALSE;
    }

    NormalizeJsonPathRule(Rule, normalizedRule, ARRAYSIZE(normalizedRule));
    return GlobMatchNoCase(Path, normalizedRule);
}

static
BOOLEAN
UrlContainsMatchingJsonPath(
    _In_z_ PCWSTR Text,
    _In_z_ PCWSTR Rule
    )
{
    PCWSTR marker;

    if (Text == NULL || Rule == NULL || Rule[0] == L'\0') {
        return FALSE;
    }

    marker = Text;
    while ((marker = wcsstr(marker, L"JSONPATH=")) != NULL) {
        WCHAR path[PS_POLICY_JSON_PATH_CHARS];
        PCWSTR start = marker + 9;
        PCWSTR end = wcsstr(start, L" JSONVAL=");
        SIZE_T copyLen;

        if (end == NULL) {
            end = start + wcslen(start);
        }

        copyLen = (SIZE_T)(end - start);
        if (copyLen >= ARRAYSIZE(path)) {
            copyLen = ARRAYSIZE(path) - 1;
        }

        RtlCopyMemory(path, start, copyLen * sizeof(WCHAR));
        path[copyLen] = L'\0';

        if (JsonPathRuleMatchesNoCase(path, Rule)) {
            return TRUE;
        }

        marker = end;
    }

    return FALSE;
}

typedef struct _JSON_PREDICATE_RULE {
    WCHAR PrefixPattern[PS_POLICY_JSON_PATH_CHARS];
    ULONG GroupCount;
    struct {
        ULONG ConditionCount;
        struct {
            WCHAR Key[PS_JSON_KEY_LEN];
            WCHAR ValuePattern[PS_JSON_VALUE_LEN];
            JSON_PREDICATE_OP Operator;
        } Conditions[PS_MAX_JSON_PREDICATE_COND];
    } Groups[PS_MAX_JSON_PREDICATE_GROUP];
    WCHAR Suffix[PS_POLICY_JSON_PATH_CHARS];
} JSON_PREDICATE_RULE, *PJSON_PREDICATE_RULE;

typedef struct _JSON_PREDICATE_MATCH_SCRATCH {
    JSON_PREDICATE_RULE Parsed;
    WCHAR Key[PS_JSON_KEY_LEN];
    WCHAR Path[PS_POLICY_JSON_PATH_CHARS];
    WCHAR Value[PS_JSON_VALUE_LEN];
    WCHAR ObjectPath[PS_POLICY_JSON_PATH_CHARS];
    WCHAR SiblingPath[PS_POLICY_JSON_PATH_CHARS];
    WCHAR SiblingKey[PS_JSON_KEY_LEN];
    WCHAR SiblingValue[PS_JSON_VALUE_LEN];
    WCHAR SiblingPathValue[PS_POLICY_JSON_PATH_CHARS];
} JSON_PREDICATE_MATCH_SCRATCH, *PJSON_PREDICATE_MATCH_SCRATCH;

static
BOOLEAN
CopyTrimmedWideSpan(
    _In_reads_(Length) PCWSTR Start,
    _In_ SIZE_T Length,
    _Out_writes_(OutChars) PWCHAR Out,
    _In_ SIZE_T OutChars
    )
{
    SIZE_T begin = 0;
    SIZE_T end = Length;
    SIZE_T copyLen;

    if (Start == NULL || Out == NULL || OutChars == 0) {
        return FALSE;
    }

    while (begin < Length &&
           (Start[begin] == L' ' || Start[begin] == L'\t' ||
            Start[begin] == L'\r' || Start[begin] == L'\n')) {
        begin++;
    }
    while (end > begin &&
           (Start[end - 1] == L' ' || Start[end - 1] == L'\t' ||
            Start[end - 1] == L'\r' || Start[end - 1] == L'\n')) {
        end--;
    }

    copyLen = end - begin;
    if (copyLen == 0 || copyLen >= OutChars) {
        return FALSE;
    }

    RtlCopyMemory(Out, Start + begin, copyLen * sizeof(WCHAR));
    Out[copyLen] = L'\0';
    return TRUE;
}

static
BOOLEAN
ParseSignedNumberWide(
    _In_z_ PCWSTR Text,
    _Out_ LONGLONG* Value
    )
{
    LONGLONG result = 0;
    BOOLEAN negative = FALSE;
    PCWSTR cursor;

    if (Text == NULL || Value == NULL || Text[0] == L'\0') {
        return FALSE;
    }

    cursor = Text;
    if (*cursor == L'-') {
        negative = TRUE;
        cursor++;
    }
    if (*cursor == L'\0') {
        return FALSE;
    }

    while (*cursor != L'\0') {
        if (*cursor < L'0' || *cursor > L'9') {
            return FALSE;
        }
        result = (result * 10) + (*cursor - L'0');
        cursor++;
    }

    *Value = negative ? -result : result;
    return TRUE;
}

static
BOOLEAN
ParseJsonPredicateRule(
    _In_z_ PCWSTR Rule,
    _Out_ PJSON_PREDICATE_RULE Parsed
    )
{
    PCWSTR leftBrace;
    PCWSTR rightBrace;
    SIZE_T prefixLen;
    SIZE_T suffixLen;
    WCHAR predicateBuffer[PS_POLICY_JSON_PATH_CHARS];
    SIZE_T predicateLen;
    PWCHAR groupCursor;

    if (Rule == NULL || Parsed == NULL) {
        return FALSE;
    }

    RtlZeroMemory(Parsed, sizeof(*Parsed));
    leftBrace = wcschr(Rule, L'{');
    if (leftBrace == NULL) {
        return FALSE;
    }

    rightBrace = wcschr(leftBrace + 1, L'}');
    if (rightBrace == NULL || rightBrace <= leftBrace + 1) {
        return FALSE;
    }

    prefixLen = (SIZE_T)(leftBrace - Rule);
    suffixLen = wcslen(rightBrace + 1);
    predicateLen = (SIZE_T)(rightBrace - (leftBrace + 1));

    if (predicateLen == 0 || suffixLen == 0 ||
        prefixLen >= ARRAYSIZE(Parsed->PrefixPattern) ||
        predicateLen >= ARRAYSIZE(predicateBuffer) ||
        suffixLen >= ARRAYSIZE(Parsed->Suffix)) {
        return FALSE;
    }

    RtlCopyMemory(Parsed->PrefixPattern, Rule, prefixLen * sizeof(WCHAR));
    Parsed->PrefixPattern[prefixLen] = L'\0';
    RtlCopyMemory(Parsed->Suffix, rightBrace + 1, suffixLen * sizeof(WCHAR));
    Parsed->Suffix[suffixLen] = L'\0';
    RtlCopyMemory(predicateBuffer, leftBrace + 1, predicateLen * sizeof(WCHAR));
    predicateBuffer[predicateLen] = L'\0';

    groupCursor = predicateBuffer;
    while (*groupCursor != L'\0') {
        PWCHAR nextOr = wcsstr(groupCursor, L"||");
        SIZE_T groupLen = nextOr != NULL ? (SIZE_T)(nextOr - groupCursor) : wcslen(groupCursor);
        WCHAR groupBuffer[PS_POLICY_JSON_PATH_CHARS];
        PWCHAR cursor;

        if (Parsed->GroupCount >= PS_MAX_JSON_PREDICATE_GROUP ||
            groupLen == 0 || groupLen >= ARRAYSIZE(groupBuffer)) {
            return FALSE;
        }

        RtlCopyMemory(groupBuffer, groupCursor, groupLen * sizeof(WCHAR));
        groupBuffer[groupLen] = L'\0';
        cursor = groupBuffer;

        while (*cursor != L'\0') {
            PWCHAR nextAnd = wcsstr(cursor, L"&&");
            PWCHAR nextComma = wcschr(cursor, L',');
            PWCHAR split = NULL;
            PWCHAR opPosition = NULL;
            JSON_PREDICATE_OP op = JsonPredicateOpEq;
            SIZE_T tokenLen;
            ULONG condIndex = Parsed->Groups[Parsed->GroupCount].ConditionCount;

            if (nextAnd != NULL && nextComma != NULL) {
                split = nextAnd < nextComma ? nextAnd : nextComma;
            } else if (nextAnd != NULL) {
                split = nextAnd;
            } else {
                split = nextComma;
            }

            tokenLen = split != NULL ? (SIZE_T)(split - cursor) : wcslen(cursor);
            if ((opPosition = wcsstr(cursor, L"!=")) != NULL &&
                (SIZE_T)(opPosition - cursor) < tokenLen) {
                op = JsonPredicateOpNe;
            } else if ((opPosition = wcsstr(cursor, L">=")) != NULL &&
                       (SIZE_T)(opPosition - cursor) < tokenLen) {
                op = JsonPredicateOpGe;
            } else if ((opPosition = wcsstr(cursor, L"<=")) != NULL &&
                       (SIZE_T)(opPosition - cursor) < tokenLen) {
                op = JsonPredicateOpLe;
            } else if ((opPosition = wcschr(cursor, L'>')) != NULL &&
                       (SIZE_T)(opPosition - cursor) < tokenLen) {
                op = JsonPredicateOpGt;
            } else if ((opPosition = wcschr(cursor, L'<')) != NULL &&
                       (SIZE_T)(opPosition - cursor) < tokenLen) {
                op = JsonPredicateOpLt;
            } else if ((opPosition = wcschr(cursor, L'=')) != NULL &&
                       (SIZE_T)(opPosition - cursor) < tokenLen) {
                op = JsonPredicateOpEq;
            }

            if (opPosition == NULL || (SIZE_T)(opPosition - cursor) >= tokenLen ||
                condIndex >= PS_MAX_JSON_PREDICATE_COND) {
                return FALSE;
            }

            if (!CopyTrimmedWideSpan(
                    cursor,
                    (SIZE_T)(opPosition - cursor),
                    Parsed->Groups[Parsed->GroupCount].Conditions[condIndex].Key,
                    ARRAYSIZE(Parsed->Groups[Parsed->GroupCount].Conditions[condIndex].Key)) ||
                !CopyTrimmedWideSpan(
                    opPosition + ((op == JsonPredicateOpNe || op == JsonPredicateOpGe || op == JsonPredicateOpLe) ? 2 : 1),
                    tokenLen - (SIZE_T)((opPosition + ((op == JsonPredicateOpNe || op == JsonPredicateOpGe || op == JsonPredicateOpLe) ? 2 : 1)) - cursor),
                    Parsed->Groups[Parsed->GroupCount].Conditions[condIndex].ValuePattern,
                    ARRAYSIZE(Parsed->Groups[Parsed->GroupCount].Conditions[condIndex].ValuePattern))) {
                return FALSE;
            }
            Parsed->Groups[Parsed->GroupCount].Conditions[condIndex].Operator = op;
            Parsed->Groups[Parsed->GroupCount].ConditionCount++;

            if (split == NULL) {
                break;
            }
            cursor = split + ((*split == L',') ? 1 : 2);
        }

        if (Parsed->Groups[Parsed->GroupCount].ConditionCount == 0) {
            return FALSE;
        }
        Parsed->GroupCount++;

        if (nextOr == NULL) {
            break;
        }
        groupCursor = nextOr + 2;
    }

    return Parsed->GroupCount > 0;
}

static
BOOLEAN
EndsWithNoCaseWide(
    _In_z_ PCWSTR Text,
    _In_z_ PCWSTR Suffix
    )
{
    SIZE_T textLen;
    SIZE_T suffixLen;

    if (Text == NULL || Suffix == NULL) {
        return FALSE;
    }

    textLen = wcslen(Text);
    suffixLen = wcslen(Suffix);
    if (suffixLen == 0 || suffixLen > textLen) {
        return FALSE;
    }

    return _wcsnicmp(Text + (textLen - suffixLen), Suffix, suffixLen) == 0;
}

static
BOOLEAN
JsonPredicateValueMatches(
    _In_z_ PCWSTR CandidateValue,
    _In_ JSON_PREDICATE_OP Operator,
    _In_z_ PCWSTR ExpectedValue
    )
{
    LONGLONG left;
    LONGLONG right;

    if (CandidateValue == NULL || ExpectedValue == NULL) {
        return FALSE;
    }

    switch (Operator) {
        case JsonPredicateOpEq:
            return GlobMatchNoCase(CandidateValue, ExpectedValue) ||
                ContainsNoCase(CandidateValue, ExpectedValue);
        case JsonPredicateOpNe:
            return !(GlobMatchNoCase(CandidateValue, ExpectedValue) ||
                ContainsNoCase(CandidateValue, ExpectedValue));
        case JsonPredicateOpGt:
        case JsonPredicateOpGe:
        case JsonPredicateOpLt:
        case JsonPredicateOpLe:
            if (!ParseSignedNumberWide(CandidateValue, &left) ||
                !ParseSignedNumberWide(ExpectedValue, &right)) {
                return FALSE;
            }
            if (Operator == JsonPredicateOpGt) return left > right;
            if (Operator == JsonPredicateOpGe) return left >= right;
            if (Operator == JsonPredicateOpLt) return left < right;
            return left <= right;
        default:
            return FALSE;
    }
}

static
BOOLEAN
NextJsonSemanticEntry(
    _In_z_ PCWSTR Text,
    _Inout_ PCWSTR* Cursor,
    _Out_writes_(PS_JSON_KEY_LEN) PWCHAR Key,
    _Out_writes_(PS_POLICY_JSON_PATH_CHARS) PWCHAR Path,
    _Out_writes_(PS_JSON_VALUE_LEN) PWCHAR Value
    )
{
    PCWSTR marker;
    PCWSTR keyStart;
    PCWSTR pathMarker;
    PCWSTR pathStart;
    PCWSTR valueMarker;
    PCWSTR valueStart;
    PCWSTR nextMarker;
    SIZE_T keyLen;
    SIZE_T pathLen;
    SIZE_T valueLen;

    if (Text == NULL || Cursor == NULL || Key == NULL || Path == NULL || Value == NULL) {
        return FALSE;
    }

    marker = *Cursor != NULL ? wcsstr(*Cursor, L"JSONKEY=") : wcsstr(Text, L"JSONKEY=");
    if (marker == NULL) {
        return FALSE;
    }

    keyStart = marker + 8;
    pathMarker = wcsstr(keyStart, L" JSONPATH=");
    if (pathMarker == NULL) {
        return FALSE;
    }
    pathStart = pathMarker + 10;
    valueMarker = wcsstr(pathStart, L" JSONVAL=");
    if (valueMarker == NULL) {
        return FALSE;
    }
    valueStart = valueMarker + 9;
    nextMarker = wcsstr(valueStart, L" | JSONKEY=");

    keyLen = (SIZE_T)(pathMarker - keyStart);
    pathLen = (SIZE_T)(valueMarker - pathStart);
    valueLen = nextMarker != NULL ? (SIZE_T)(nextMarker - valueStart) : wcslen(valueStart);

    if (keyLen >= PS_JSON_KEY_LEN) keyLen = PS_JSON_KEY_LEN - 1;
    if (pathLen >= PS_POLICY_JSON_PATH_CHARS) pathLen = PS_POLICY_JSON_PATH_CHARS - 1;
    if (valueLen >= PS_JSON_VALUE_LEN) valueLen = PS_JSON_VALUE_LEN - 1;

    RtlCopyMemory(Key, keyStart, keyLen * sizeof(WCHAR));
    Key[keyLen] = L'\0';
    RtlCopyMemory(Path, pathStart, pathLen * sizeof(WCHAR));
    Path[pathLen] = L'\0';
    RtlCopyMemory(Value, valueStart, valueLen * sizeof(WCHAR));
    Value[valueLen] = L'\0';

    *Cursor = nextMarker != NULL ? nextMarker + 3 : valueStart + valueLen;
    return TRUE;
}

static
BOOLEAN
UrlContainsMatchingJsonPredicatePath(
    _In_z_ PCWSTR Text,
    _In_z_ PCWSTR Rule
    )
{
    PJSON_PREDICATE_MATCH_SCRATCH scratch;
    PCWSTR cursor = Text;
    BOOLEAN matched = FALSE;

    scratch = (PJSON_PREDICATE_MATCH_SCRATCH)ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(*scratch),
        'jPsP');
    if (scratch == NULL) {
        return FALSE;
    }
    if (!ParseJsonPredicateRule(Rule, &scratch->Parsed)) {
        ExFreePool(scratch);
        return FALSE;
    }

    while (TRUE) {
        SIZE_T pathLen;
        SIZE_T suffixLen;
        PCWSTR siblingCursor;

        if (!NextJsonSemanticEntry(
                Text,
                &cursor,
                scratch->Key,
                scratch->Path,
                scratch->Value)) {
            break;
        }

        if (!EndsWithNoCaseWide(scratch->Path, scratch->Parsed.Suffix)) {
            continue;
        }

        pathLen = wcslen(scratch->Path);
        suffixLen = wcslen(scratch->Parsed.Suffix);
        if (pathLen < suffixLen || pathLen - suffixLen >= ARRAYSIZE(scratch->ObjectPath)) {
            continue;
        }

        RtlCopyMemory(scratch->ObjectPath, scratch->Path, (pathLen - suffixLen) * sizeof(WCHAR));
        scratch->ObjectPath[pathLen - suffixLen] = L'\0';
        if (!JsonPathRuleMatchesNoCase(scratch->ObjectPath, scratch->Parsed.PrefixPattern)) {
            continue;
        }

        {
            ULONG groupIndex;

            for (groupIndex = 0; groupIndex < scratch->Parsed.GroupCount; groupIndex++) {
                ULONG condIndex;
                BOOLEAN allMatched = TRUE;

                for (condIndex = 0; condIndex < scratch->Parsed.Groups[groupIndex].ConditionCount; condIndex++) {
                    BOOLEAN conditionMatched = FALSE;

                    if (scratch->ObjectPath[0] == L'\0') {
                        RtlStringCchCopyW(
                            scratch->SiblingPath,
                            ARRAYSIZE(scratch->SiblingPath),
                            scratch->Parsed.Groups[groupIndex].Conditions[condIndex].Key);
                    } else {
                        RtlStringCchPrintfW(
                            scratch->SiblingPath,
                            ARRAYSIZE(scratch->SiblingPath),
                            L"%s.%s",
                            scratch->ObjectPath,
                            scratch->Parsed.Groups[groupIndex].Conditions[condIndex].Key);
                    }

                    siblingCursor = Text;
                    while (TRUE) {
                        if (!NextJsonSemanticEntry(
                                Text,
                                &siblingCursor,
                                scratch->SiblingKey,
                                scratch->SiblingPathValue,
                                scratch->SiblingValue)) {
                            break;
                        }

                        if (_wcsicmp(scratch->SiblingPathValue, scratch->SiblingPath) == 0 &&
                            JsonPredicateValueMatches(
                                scratch->SiblingValue,
                                scratch->Parsed.Groups[groupIndex].Conditions[condIndex].Operator,
                                scratch->Parsed.Groups[groupIndex].Conditions[condIndex].ValuePattern)) {
                            conditionMatched = TRUE;
                            break;
                        }
                    }

                    if (!conditionMatched) {
                        allMatched = FALSE;
                        break;
                    }
                }

                if (allMatched) {
                    matched = TRUE;
                    goto Exit;
                }
            }
        }
    }

Exit:
    ExFreePool(scratch);
    return matched;
}

static
VOID
PolicyEngineSnapshotPolicy(
    _Out_ PS_POLICY_DATA* Snapshot
    )
{
    KIRQL oldIrql;

    if (Snapshot == NULL) {
        return;
    }

    KeAcquireSpinLock(&gPolicyState.Lock, &oldIrql);
    RtlCopyMemory(Snapshot, &gPolicyState.Policy, sizeof(*Snapshot));
    KeReleaseSpinLock(&gPolicyState.Lock, oldIrql);
}

static
PPS_POLICY_DATA
PolicyEngineAllocateSnapshot(VOID)
{
    PPS_POLICY_DATA snapshot;

    if (!gPolicyState.SnapshotLookasideInitialized) return NULL;
    snapshot = (PPS_POLICY_DATA)ExAllocateFromNPagedLookasideList(&gPolicyState.SnapshotLookaside);
    if (snapshot != NULL) PolicyEngineSnapshotPolicy(snapshot);
    return snapshot;
}

static
VOID
PolicyEngineFreeSnapshot(
    _In_opt_ PPS_POLICY_DATA Snapshot
    )
{
    if (Snapshot != NULL && gPolicyState.SnapshotLookasideInitialized) {
        ExFreeToNPagedLookasideList(&gPolicyState.SnapshotLookaside, Snapshot);
    }
}

static
BOOLEAN
PolicyMatchesProcessName(
    _In_ const PS_POLICY_DATA* Policy,
    _In_ PCWSTR ProcessName
    )
{
    ULONG count;
    ULONG i;

    if (Policy == NULL || ProcessName == NULL) {
        return FALSE;
    }

    count = Policy->BlockedProcCount;
    if (count > PS_MAX_BLOCK_PROC) count = PS_MAX_BLOCK_PROC;
    for (i = 0; i < count; i++) {
        if (_wcsicmp(ProcessName, Policy->BlockedProcesses[i]) == 0) {
            return TRUE;
        }
    }

    return FALSE;
}

NTSTATUS
PolicyEngineInitialize(VOID)
{
    RtlZeroMemory(&gPolicyState, sizeof(gPolicyState));
    KeInitializeSpinLock(&gPolicyState.Lock);
    ExInitializeNPagedLookasideList(
        &gPolicyState.SnapshotLookaside,
        NULL,
        NULL,
        POOL_NX_ALLOCATION,
        sizeof(PS_POLICY_DATA),
        'sPsP',
        0);
    gPolicyState.SnapshotLookasideInitialized = TRUE;
    gPolicyState.Policy.FileFilterEnabled = 1;
    gPolicyState.Policy.NetworkFilterEnabled = 1;
    gPolicyState.Policy.AuditEnabled = 1;
    gPolicyState.Quarantine.Enabled = 0;
    RtlStringCchCopyW(
        gPolicyState.Quarantine.RootDirectory,
        ARRAYSIZE(gPolicyState.Quarantine.RootDirectory),
        gDefaultQuarantineRoot);
    gPolicyState.Initialized = TRUE;

    DLP_LOG(DLP_DEBUG_INFO, "Policy engine initialized");
    return STATUS_SUCCESS;
}

VOID
PolicyEngineCleanup(VOID)
{
    KIRQL oldIrql;

    KeAcquireSpinLock(&gPolicyState.Lock, &oldIrql);
    RtlZeroMemory(&gPolicyState.Policy, sizeof(gPolicyState.Policy));
    RtlZeroMemory(&gPolicyState.Quarantine, sizeof(gPolicyState.Quarantine));
    gPolicyState.Initialized = FALSE;
    KeReleaseSpinLock(&gPolicyState.Lock, oldIrql);

    if (gPolicyState.SnapshotLookasideInitialized) {
        ExDeleteNPagedLookasideList(&gPolicyState.SnapshotLookaside);
        gPolicyState.SnapshotLookasideInitialized = FALSE;
    }

    DLP_LOG(DLP_DEBUG_INFO, "Policy engine cleaned up");
}

BOOLEAN
PolicyEngineIsAuditEnabled(VOID)
{
    KIRQL oldIrql;
    BOOLEAN enabled;

    KeAcquireSpinLock(&gPolicyState.Lock, &oldIrql);
    enabled = gPolicyState.Initialized && gPolicyState.Policy.AuditEnabled != 0;
    KeReleaseSpinLock(&gPolicyState.Lock, oldIrql);
    return enabled;
}

NTSTATUS
PolicyEngineSetPolicy(
    _In_ PPOLICY_COMMAND Command
    )
{
    KIRQL oldIrql;
    PPS_POLICY_DATA snapshot;

    if (Command == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Command->PolicySize < sizeof(PS_POLICY_DATA)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    snapshot = (PPS_POLICY_DATA)Command->PolicyData;
    KeAcquireSpinLock(&gPolicyState.Lock, &oldIrql);
    RtlCopyMemory(&gPolicyState.Policy, Command->PolicyData, sizeof(PS_POLICY_DATA));
    KeReleaseSpinLock(&gPolicyState.Lock, oldIrql);

    DLP_LOG(
        DLP_DEBUG_INFO,
        "Policy updated: file=%lu net=%lu ext=%lu proc=%lu port=%lu domain=%lu url=%lu",
        snapshot->FileFilterEnabled,
        snapshot->NetworkFilterEnabled,
        snapshot->BlockedExtCount,
        snapshot->BlockedProcCount,
        snapshot->BlockedPortCount,
        snapshot->BlockedDomainCount,
        snapshot->BlockedUrlCount);

    return STATUS_SUCCESS;
}

NTSTATUS
PolicyEngineGetPolicy(
    _Out_writes_bytes_(bufferSize) PVOID Buffer,
    _In_ ULONG bufferSize,
    _Out_ PULONG bytesReturned
    )
{
    KIRQL oldIrql;

    if (Buffer == NULL || bytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (bufferSize < sizeof(PS_POLICY_DATA)) {
        *bytesReturned = 0;
        return STATUS_BUFFER_TOO_SMALL;
    }

    KeAcquireSpinLock(&gPolicyState.Lock, &oldIrql);
    RtlCopyMemory(Buffer, &gPolicyState.Policy, sizeof(PS_POLICY_DATA));
    KeReleaseSpinLock(&gPolicyState.Lock, oldIrql);

    *bytesReturned = sizeof(PS_POLICY_DATA);
    return STATUS_SUCCESS;
}

NTSTATUS
PolicyEngineReload(VOID)
{
    DLP_LOG(DLP_DEBUG_INFO, "Policy reloaded");
    return STATUS_SUCCESS;
}

NTSTATUS
PolicyEngineSetQuarantineSettings(
    _In_ const PS_QUARANTINE_SETTINGS* Settings
    )
{
    KIRQL oldIrql;

    if (Settings == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    KeAcquireSpinLock(&gPolicyState.Lock, &oldIrql);
    RtlCopyMemory(&gPolicyState.Quarantine, Settings, sizeof(gPolicyState.Quarantine));
    gPolicyState.Quarantine.RootDirectory[ARRAYSIZE(gPolicyState.Quarantine.RootDirectory) - 1] = L'\0';
    KeReleaseSpinLock(&gPolicyState.Lock, oldIrql);

    return STATUS_SUCCESS;
}

NTSTATUS
PolicyEngineGetQuarantineSettings(
    _Out_ PS_QUARANTINE_SETTINGS* Settings
    )
{
    KIRQL oldIrql;

    if (Settings == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    KeAcquireSpinLock(&gPolicyState.Lock, &oldIrql);
    RtlCopyMemory(Settings, &gPolicyState.Quarantine, sizeof(*Settings));
    KeReleaseSpinLock(&gPolicyState.Lock, oldIrql);

    return STATUS_SUCCESS;
}

BOOLEAN
PolicyEngineIsProcessBlocked(
    _In_ PCWSTR ProcessName
    )
{
    PPS_POLICY_DATA snapshot;
    BOOLEAN blocked;

    if (ProcessName == NULL) return FALSE;

    snapshot = PolicyEngineAllocateSnapshot();
    if (snapshot == NULL) return FALSE;
    blocked = PolicyMatchesProcessName(snapshot, ProcessName);
    PolicyEngineFreeSnapshot(snapshot);
    return blocked;
}

ACTION_RESULT
PolicyEngineQueryFileAction(
    _In_ ULONG ProcessId,
    _In_ PCWSTR FileName,
    _In_ DLP_EVENT_TYPE EventType
    )
{
    ACTION_RESULT action = ActionLogged;
    PPS_POLICY_DATA snapshot;
    ULONG extCount;
    ULONG i;

    UNREFERENCED_PARAMETER(EventType);

    if (ProtectShouldBypassDlp(ProcessId)) return ActionAllowed;

    if (FileName == NULL) return ActionLogged;

    snapshot = PolicyEngineAllocateSnapshot();
    if (snapshot == NULL) return ActionLogged;
    if (!snapshot->FileFilterEnabled) {
        PolicyEngineFreeSnapshot(snapshot);
        return ActionAllowed;
    }

    extCount = snapshot->BlockedExtCount;
    if (extCount > PS_MAX_BLOCK_EXT) extCount = PS_MAX_BLOCK_EXT;
    for (i = 0; i < extCount; i++) {
        if (EndsWithNoCase(FileName, snapshot->BlockedExtensions[i])) {
            action = ActionBlocked;
            break;
        }
    }

    PolicyEngineFreeSnapshot(snapshot);
    return action;
}

ACTION_RESULT
PolicyEngineQueryNetAction(
    _In_ ULONG ProcessId,
    _In_ UINT16 RemotePort,
    _In_ PCWSTR Url,
    _In_ DLP_EVENT_TYPE EventType
    )
{
    ACTION_RESULT action = ActionLogged;
    PPS_POLICY_DATA snapshot;
    WCHAR processName[260];
    ULONG portCount;
    ULONG domainCount;
    ULONG urlCount;
    ULONG ftpCommandCount;
    ULONG ftpPathCount;
    ULONG ftpContentCount;
    ULONG headerCount;
    ULONG trailerCount;
    ULONG bodyPatternCount;
    ULONG jsonKeyCount;
    ULONG jsonPathCount;
    ULONG jsonValueCount;
    ULONG i;

    if (ProtectShouldBypassDlp(ProcessId)) {
        return ActionAllowed;
    }

    snapshot = PolicyEngineAllocateSnapshot();
    if (snapshot == NULL) return ActionLogged;
    if (!snapshot->NetworkFilterEnabled) {
        PolicyEngineFreeSnapshot(snapshot);
        return ActionAllowed;
    }

    portCount = snapshot->BlockedPortCount;
    if (portCount > PS_MAX_BLOCK_PORT) portCount = PS_MAX_BLOCK_PORT;
    for (i = 0; i < portCount; i++) {
        if (snapshot->BlockedPorts[i] == RemotePort) {
            action = ActionBlocked;
            break;
        }
    }

    if (action != ActionBlocked && Url != NULL) {
        domainCount = snapshot->BlockedDomainCount;
        urlCount = snapshot->BlockedUrlCount;
        if (domainCount > PS_MAX_BLOCK_DOMAIN) domainCount = PS_MAX_BLOCK_DOMAIN;
        if (urlCount > PS_MAX_BLOCK_URL) urlCount = PS_MAX_BLOCK_URL;

        if (EventType == EventHttpRequest) {
            headerCount = snapshot->BlockedHttpHeaderCount;
            bodyPatternCount = snapshot->BlockedHttpBodyPatternCount;
            if (headerCount > PS_MAX_BLOCK_HTTP_HEADER) headerCount = PS_MAX_BLOCK_HTTP_HEADER;
            if (bodyPatternCount > PS_MAX_BLOCK_HTTP_BODY) bodyPatternCount = PS_MAX_BLOCK_HTTP_BODY;
            for (i = 0; i < urlCount; i++) {
                if (ContainsNoCase(Url, snapshot->BlockedUrls[i])) {
                    action = ActionBlocked;
                    break;
                }
            }
            if (action != ActionBlocked) {
                for (i = 0; i < domainCount; i++) {
                    if (DomainMatchesNoCase(Url, snapshot->BlockedDomains[i])) {
                        action = ActionBlocked;
                        break;
                    }
                }
            }
            if (action != ActionBlocked) {
                for (i = 0; i < headerCount; i++) {
                    if (ContainsNoCase(Url, snapshot->BlockedHttpHeaders[i])) {
                        action = ActionBlocked;
                        break;
                    }
                }
            }
            if (action != ActionBlocked && ContainsNoCase(Url, L"BODY ")) {
                for (i = 0; i < bodyPatternCount; i++) {
                    if (ContainsNoCase(Url, snapshot->BlockedHttpBodyPatterns[i])) {
                        action = ActionBlocked;
                        break;
                    }
                }
                if (action != ActionBlocked && ContainsNoCase(Url, L"JSONKEY=")) {
                    jsonKeyCount = snapshot->BlockedJsonKeyCount;
                    jsonPathCount = snapshot->BlockedJsonPathCount;
                    jsonValueCount = snapshot->BlockedJsonValueCount;
                    if (jsonKeyCount > PS_MAX_BLOCK_JSON_KEY) jsonKeyCount = PS_MAX_BLOCK_JSON_KEY;
                    if (jsonPathCount > PS_MAX_BLOCK_JSON_PATH) jsonPathCount = PS_MAX_BLOCK_JSON_PATH;
                    if (jsonValueCount > PS_MAX_BLOCK_JSON_VALUE) jsonValueCount = PS_MAX_BLOCK_JSON_VALUE;
                    for (i = 0; i < jsonKeyCount; i++) {
                        if (ContainsNoCase(Url, snapshot->BlockedJsonKeys[i])) {
                            action = ActionBlocked;
                            break;
                        }
                    }
                    if (action != ActionBlocked) {
                        for (i = 0; i < jsonPathCount; i++) {
                            if (UrlContainsMatchingJsonPath(Url, snapshot->BlockedJsonPaths[i]) ||
                                UrlContainsMatchingJsonPredicatePath(Url, snapshot->BlockedJsonPaths[i]) ||
                                ContainsNoCase(Url, snapshot->BlockedJsonPaths[i])) {
                                action = ActionBlocked;
                                break;
                            }
                        }
                    }
                    if (action != ActionBlocked) {
                        for (i = 0; i < jsonValueCount; i++) {
                            if (ContainsNoCase(Url, snapshot->BlockedJsonValues[i])) {
                                action = ActionBlocked;
                                break;
                            }
                        }
                    }
                }
            }
        } else if (EventType == EventSniCapture) {
            for (i = 0; i < domainCount; i++) {
                if (DomainMatchesNoCase(Url, snapshot->BlockedDomains[i])) {
                    action = ActionBlocked;
                    break;
                }
            }
        } else if (EventType == EventFtpCommand || EventType == EventHttpResponse) {
            for (i = 0; i < urlCount; i++) {
                if (ContainsNoCase(Url, snapshot->BlockedUrls[i])) {
                    action = ActionBlocked;
                    break;
                }
            }
            if (action != ActionBlocked && EventType == EventHttpResponse) {
                headerCount = snapshot->BlockedHttpHeaderCount;
                trailerCount = snapshot->BlockedHttpTrailerCount;
                bodyPatternCount = snapshot->BlockedHttpBodyPatternCount;
                if (headerCount > PS_MAX_BLOCK_HTTP_HEADER) headerCount = PS_MAX_BLOCK_HTTP_HEADER;
                if (trailerCount > PS_MAX_BLOCK_HTTP_TRAILER) trailerCount = PS_MAX_BLOCK_HTTP_TRAILER;
                if (bodyPatternCount > PS_MAX_BLOCK_HTTP_BODY) bodyPatternCount = PS_MAX_BLOCK_HTTP_BODY;
                if (ContainsNoCase(Url, L"TRAILER ")) {
                    for (i = 0; i < trailerCount; i++) {
                        if (ContainsNoCase(Url, snapshot->BlockedHttpTrailers[i])) {
                            action = ActionBlocked;
                            break;
                        }
                    }
                } else if (ContainsNoCase(Url, L"BODY ")) {
                    for (i = 0; i < bodyPatternCount; i++) {
                        if (ContainsNoCase(Url, snapshot->BlockedHttpBodyPatterns[i])) {
                            action = ActionBlocked;
                            break;
                        }
                    }
                    if (action != ActionBlocked && ContainsNoCase(Url, L"JSONKEY=")) {
                        jsonKeyCount = snapshot->BlockedJsonKeyCount;
                        jsonPathCount = snapshot->BlockedJsonPathCount;
                        jsonValueCount = snapshot->BlockedJsonValueCount;
                        if (jsonKeyCount > PS_MAX_BLOCK_JSON_KEY) jsonKeyCount = PS_MAX_BLOCK_JSON_KEY;
                        if (jsonPathCount > PS_MAX_BLOCK_JSON_PATH) jsonPathCount = PS_MAX_BLOCK_JSON_PATH;
                        if (jsonValueCount > PS_MAX_BLOCK_JSON_VALUE) jsonValueCount = PS_MAX_BLOCK_JSON_VALUE;
                        for (i = 0; i < jsonKeyCount; i++) {
                            if (ContainsNoCase(Url, snapshot->BlockedJsonKeys[i])) {
                                action = ActionBlocked;
                                break;
                            }
                        }
                        if (action != ActionBlocked) {
                            for (i = 0; i < jsonPathCount; i++) {
                                if (UrlContainsMatchingJsonPath(Url, snapshot->BlockedJsonPaths[i]) ||
                                    UrlContainsMatchingJsonPredicatePath(Url, snapshot->BlockedJsonPaths[i]) ||
                                    ContainsNoCase(Url, snapshot->BlockedJsonPaths[i])) {
                                    action = ActionBlocked;
                                    break;
                                }
                            }
                        }
                        if (action != ActionBlocked) {
                            for (i = 0; i < jsonValueCount; i++) {
                                if (ContainsNoCase(Url, snapshot->BlockedJsonValues[i])) {
                                    action = ActionBlocked;
                                    break;
                                }
                            }
                        }
                    }
                } else {
                    for (i = 0; i < headerCount; i++) {
                        if (ContainsNoCase(Url, snapshot->BlockedHttpHeaders[i])) {
                            action = ActionBlocked;
                            break;
                        }
                    }
                }
            }
            if (action != ActionBlocked && EventType == EventFtpCommand) {
                ftpCommandCount = snapshot->BlockedFtpCommandCount;
                ftpPathCount = snapshot->BlockedFtpPathCount;
                ftpContentCount = snapshot->BlockedFtpContentPatternCount;
                if (ftpCommandCount > PS_MAX_BLOCK_FTP_CMD) ftpCommandCount = PS_MAX_BLOCK_FTP_CMD;
                if (ftpPathCount > PS_MAX_BLOCK_FTP_PATH) ftpPathCount = PS_MAX_BLOCK_FTP_PATH;
                if (ftpContentCount > PS_MAX_BLOCK_FTP_CONTENT) ftpContentCount = PS_MAX_BLOCK_FTP_CONTENT;
                for (i = 0; i < ftpCommandCount; i++) {
                    if (FtpCommandMatchesNoCase(Url, snapshot->BlockedFtpCommands[i])) {
                        action = ActionBlocked;
                        break;
                    }
                }
                if (action != ActionBlocked) {
                    for (i = 0; i < ftpPathCount; i++) {
                        if (FtpPathMatchesNoCase(Url, snapshot->BlockedFtpPaths[i])) {
                            action = ActionBlocked;
                            break;
                        }
                    }
                }
                if (action != ActionBlocked) {
                    for (i = 0; i < ftpContentCount; i++) {
                        if (FtpContentMatchesNoCase(Url, snapshot->BlockedFtpContentPatterns[i])) {
                            action = ActionBlocked;
                            break;
                        }
                    }
                }
            }
        }
    }

    if (action != ActionBlocked && ProcessId != 0 &&
        KeGetCurrentIrql() <= APC_LEVEL) {
        processName[0] = L'\0';
        if (ProcessTrackerGetProcessNameById(
                (HANDLE)(ULONG_PTR)ProcessId,
                processName,
                ARRAYSIZE(processName)) > 0 &&
            PolicyMatchesProcessName(snapshot, processName)) {
            action = ActionBlocked;
        }
    }

    PolicyEngineFreeSnapshot(snapshot);
    return action;
}
