/*
 * QMud Project
 * Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
 *
 * File: MxpDiagnostics.h
 * Role: MXP diagnostic record types and helper declarations shared by parser/runtime debugging facilities.
 */

#ifndef QMUD_MXPDIAGNOSTICS_H
#define QMUD_MXPDIAGNOSTICS_H

// MXP error numbers
enum
{
	errMXP_Unknown = 1000,
	errMXP_UnterminatedElement,
	errMXP_UnterminatedComment,
	errMXP_UnterminatedEntity,
	errMXP_UnterminatedQuote,
	errMXP_EmptyElement,
	errMXP_ElementTooShort,
	errMXP_InvalidEntityName,
	errMXP_DefinitionWhenNotSecure,
	errMXP_InvalidElementName,
	errMXP_InvalidDefinition,
	errMXP_CannotRedefineElement,
	errMXP_NoTagInDefinition,
	errMXP_UnexpectedDefinitionSymbol,
	errMXP_NoClosingDefinitionQuote,
	errMXP_NoClosingDefinitionTag,
	errMXP_NoInbuiltDefinitionTag,
	errMXP_NoDefinitionTag,
	errMXP_BadVariableName,
	errMXP_UnknownElementInAttlist,
	errMXP_CannotRedefineEntity,
	errMXP_NoClosingSemicolon,
	errMXP_UnexpectedEntityArguments,
	errMXP_UnknownElement,
	errMXP_ElementWhenNotSecure,
	errMXP_NoClosingSemicolonInArgument,
	errMXP_ClosingUnknownTag,
	errMXP_UnknownColour,
	errMXP_InvalidEntityNumber,
	errMXP_DisallowedEntityNumber,
	errMXP_UnknownEntity,
	errMXP_InvalidArgumentName,
	errMXP_NoArgument,
	errMXP_PuebloOnly,
	errMXP_MXPOnly,
	errMXP_DefinitionAttemptInPueblo,
	errMXP_InvalidSupportArgument = 1044,
	errMXP_InvalidOptionArgument  = 1045,
	errMXP_DefinitionCannotCloseElement,
	errMXP_DefinitionCannotDefineElement,
	errMXP_CannotChangeOption = 1048,
	errMXP_OptionOutOfRange   = 1049,

	wrnMXP_ReplacingElement = 5000,
	wrnMXP_ManyOutstandingTags,
	wrnMXP_ArgumentNotSupplied,
	wrnMXP_ArgumentsToClosingTag,
	wrnMXP_OpenTagBlockedBySecureTag,
	wrnMXP_OpenTagNotThere,
	wrnMXP_TagOpenedInSecureMode,
	wrnMXP_ClosingOutOfSequenceTag,
	wrnMXP_OpenTagNotInOutputBuffer,
	wrnMXP_CharacterNameRequestedButNotDefined = 5009,
	wrnMXP_PasswordNotSent                     = 5010,
	wrnMXP_PasswordRequestedButNotDefined      = 5011,
	wrnMXP_TagNotImplemented,
	wrnMXP_OpenTagClosedAtEndOfLine,
	wrnMXP_TagClosedAtReset,
	wrnMXP_UnusedArgument,
	wrnMXP_NotStartingPueblo,

	infoMXP_VersionSent               = 10000,
	infoMXP_CharacterNameSent         = 10001,
	infoMXP_PasswordSent              = 10002,
	infoMXP_ScriptCollectionStarted   = 10003,
	infoMXP_ScriptCollectionCompleted = 10004,
	infoMXP_ResetReceived             = 10005,
	infoMXP_off                       = 10006,
	infoMXP_on                        = 10007,
	infoMXP_ModeChange                = 10008,
	infoMXP_SupportsSent              = 10009,
	infoMXP_OptionsSent               = 10010,
	infoMXP_AFKSent                   = 10011,
	infoMXP_OptionChanged             = 10012,

	msgMXP_CollectedElement = 20000,
	msgMXP_CollectedEntity,
	msgMXP_GotDefinition
};

#endif // QMUD_MXPDIAGNOSTICS_H
