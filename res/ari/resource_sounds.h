/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2012 - 2013, Digium, Inc.
 *
 * David M. Lee, II <dlee@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief Generated file - declares stubs to be implemented in
 * res/ari/resource_sounds.c
 *
 * Sound resources
 *
 * \author David M. Lee, II <dlee@digium.com>
 */

/*
 * !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 * !!!!!                               DO NOT EDIT                        !!!!!
 * !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 * This file is generated by a mustache template. Please see the original
 * template in rest-api-templates/ari_resource.h.mustache
 */

#ifndef _ASTERISK_RESOURCE_SOUNDS_H
#define _ASTERISK_RESOURCE_SOUNDS_H

#include "asterisk/ari.h"

/*! Argument struct for ast_ari_sounds_list() */
struct ast_ari_sounds_list_args {
	/*! Lookup sound for a specific language. */
	const char *lang;
	/*! Lookup sound in a specific format. */
	const char *format;
};
/*!
 * \brief Body parsing function for /sounds.
 * \param body The JSON body from which to parse parameters.
 * \param[out] args The args structure to parse into.
 * \retval zero on success
 * \retval non-zero on failure
 */
int ast_ari_sounds_list_parse_body(
	struct ast_json *body,
	struct ast_ari_sounds_list_args *args);

/*!
 * \brief List all sounds.
 *
 * \param headers HTTP headers
 * \param args Swagger parameters
 * \param[out] response HTTP response
 */
void ast_ari_sounds_list(struct ast_variable *headers, struct ast_ari_sounds_list_args *args, struct ast_ari_response *response);
/*! Argument struct for ast_ari_sounds_get() */
struct ast_ari_sounds_get_args {
	/*! Sound's id */
	const char *sound_id;
};
/*!
 * \brief Get a sound's details.
 *
 * \param headers HTTP headers
 * \param args Swagger parameters
 * \param[out] response HTTP response
 */
void ast_ari_sounds_get(struct ast_variable *headers, struct ast_ari_sounds_get_args *args, struct ast_ari_response *response);

#endif /* _ASTERISK_RESOURCE_SOUNDS_H */
