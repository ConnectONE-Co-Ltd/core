# -*- Mode: makesecurity-gmake; tab-width: 4; indent-tabs-mode: t -*-
#
#
# This security is part of the LibreOffice project.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# security, You can obtain one at http://mozilla.org/MPL/2.0/.
#

$(eval $(call gb_Library_Library,ucpsecurity))

$(eval $(call gb_Library_set_componentfile,ucpsecurity,ucb/source/ucp/security/ucpsecurity))

$(eval $(call gb_Library_use_external,ucpsecurity,boost_headers))

$(eval $(call gb_Library_use_externals,ucpsecurity,\
	openssl \
	openssl_headers \
))

$(eval $(call gb_Library_use_sdk_api,ucpsecurity))

$(eval $(call gb_Library_use_libraries,ucpsecurity,\
	comphelper \
	cppu \
	cppuhelper \
	sal \
	ucbhelper \
	$(gb_UWINAPI) \
	vcl \
))

$(eval $(call gb_Library_add_exception_objects,ucpsecurity,\
	ucb/source/ucp/security/bc \
	ucb/source/ucp/security/filcmd \
	ucb/source/ucp/security/filglob \
	ucb/source/ucp/security/filid \
	ucb/source/ucp/security/filinpstr \
	ucb/source/ucp/security/filinsreq \
	ucb/source/ucp/security/filnot \
	ucb/source/ucp/security/filprp \
	ucb/source/ucp/security/filrec \
	ucb/source/ucp/security/filrow \
	ucb/source/ucp/security/filrset \
	ucb/source/ucp/security/filstr \
	ucb/source/ucp/security/filtask \
	ucb/source/ucp/security/prov \
	ucb/source/ucp/security/shell \
))

# vim: set noet sw=4 ts=4:
