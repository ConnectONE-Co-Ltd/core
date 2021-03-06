/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * This file incorporates work covered by the following license notice:
 *
 *   Licensed to the Apache Software Foundation (ASF) under one or more
 *   contributor license agreements. See the NOTICE file distributed
 *   with this work for additional information regarding copyright
 *   ownership. The ASF licenses this file to you under the Apache
 *   License, Version 2.0 (the "License"); you may not use this file
 *   except in compliance with the License. You may obtain a copy of
 *   the License at http://www.apache.org/licenses/LICENSE-2.0 .
 */

#include <editsh.hxx>

#include <com/sun/star/container/XEnumerationAccess.hpp>
#include <com/sun/star/container/XContentEnumerationAccess.hpp>
#include <com/sun/star/document/XActionLockable.hpp>
#include <com/sun/star/drawing/FillStyle.hpp>
#include <com/sun/star/drawing/HomogenMatrix3.hpp>
#include <com/sun/star/drawing/LineStyle.hpp>
#include <com/sun/star/drawing/XEnhancedCustomShapeDefaulter.hpp>
#include <com/sun/star/lang/XServiceInfo.hpp>
#include <com/sun/star/style/XStyleFamiliesSupplier.hpp>
#include <com/sun/star/text/RelOrientation.hpp>
#include <com/sun/star/text/TextContentAnchorType.hpp>
#include <com/sun/star/text/VertOrientation.hpp>
#include <com/sun/star/text/WrapTextMode.hpp>
#include <com/sun/star/text/XTextContent.hpp>
#include <com/sun/star/text/XTextDocument.hpp>
#include <com/sun/star/text/XTextField.hpp>
#include <com/sun/star/text/XTextRange.hpp>
#include <com/sun/star/xml/crypto/SEInitializer.hpp>
#include <com/sun/star/rdf/XMetadatable.hpp>
#include <com/sun/star/security/DocumentDigitalSignatures.hpp>
#include <com/sun/star/security/XCertificate.hpp>

#include <basegfx/matrix/b2dhommatrix.hxx>
#include <comphelper/propertysequence.hxx>
#include <comphelper/propertyvalue.hxx>
#include <comphelper/processfactory.hxx>
#include <comphelper/sequence.hxx>
#include <comphelper/scopeguard.hxx>
#include <comphelper/string.hxx>
#include <editeng/formatbreakitem.hxx>
#include <editeng/unoprnms.hxx>
#include <sfx2/classificationhelper.hxx>
#include <svl/cryptosign.hxx>
#include <vcl/svapp.hxx>

#include <hintids.hxx>
#include <doc.hxx>
#include <IDocumentUndoRedo.hxx>
#include <edimp.hxx>
#include <ndtxt.hxx>
#include <paratr.hxx>
#include <fmtpdsc.hxx>
#include <viewopt.hxx>
#include <SwRewriter.hxx>
#include <numrule.hxx>
#include <swundo.hxx>
#include <docary.hxx>
#include <docsh.hxx>
#include <unoprnms.hxx>
#include <rootfrm.hxx>
#include <pagefrm.hxx>
#include <rdfhelper.hxx>
#include <sfx2/watermarkitem.hxx>

#include <unoparagraph.hxx>
#include <unotextrange.hxx>
#include <cppuhelper/bootstrap.hxx>
#include <modeltoviewhelper.hxx>
#include <strings.hrc>
#include <undobj.hxx>
#include <UndoParagraphSignature.hxx>
#include <txtatr.hxx>

#include <officecfg/Office/Common.hxx>
#include <com/sun/star/beans/PropertyAttribute.hpp>

#define WATERMARK_NAME "PowerPlusWaterMarkObject"

namespace
{
static const OUString MetaFilename("bails.rdf");
static const OUString MetaNS("urn:bails");
static const OUString ParagraphSignatureRDFName = "loext:paragraph:signature";
static const OUString ParagraphSignatureDateRDFName = "loext:paragraph:signature:date";
static const OUString ParagraphSignatureUsageRDFName = "loext:paragraph:signature:usage";
static const OUString ParagraphClassificationNameRDFName = "loext:paragraph:classification:name";
static const OUString ParagraphClassificationValueRDFName = "loext:paragraph:classification:value";
static const OUString MetadataFieldServiceName = "com.sun.star.text.textfield.MetadataField";
static const OUString DocInfoServiceName = "com.sun.star.text.TextField.DocInfo.Custom";

/// Find all page styles which are currently used in the document.
std::vector<OUString> lcl_getUsedPageStyles(SwViewShell const * pShell)
{
    std::vector<OUString> aReturn;

    SwRootFrame* pLayout = pShell->GetLayout();
    for (SwFrame* pFrame = pLayout->GetLower(); pFrame; pFrame = pFrame->GetNext())
    {
        SwPageFrame* pPage = static_cast<SwPageFrame*>(pFrame);
        if (const SwPageDesc *pDesc = pPage->FindPageDesc())
            aReturn.push_back(pDesc->GetName());
    }

    return aReturn;
}

/// Search for a field named rFieldName of type rServiceName in xText and return it.
uno::Reference<text::XTextField> lcl_findClassificationField(const uno::Reference<text::XText>& xText, const OUString& rServiceName, const OUString& rFieldName)
{
    uno::Reference<text::XTextField> xField;
    uno::Reference<container::XEnumerationAccess> xParagraphEnumerationAccess(xText, uno::UNO_QUERY);
    uno::Reference<container::XEnumeration> xParagraphs = xParagraphEnumerationAccess->createEnumeration();
    while (xParagraphs->hasMoreElements())
    {
        uno::Reference<container::XEnumerationAccess> xTextPortionEnumerationAccess(xParagraphs->nextElement(), uno::UNO_QUERY);
        uno::Reference<container::XEnumeration> xTextPortions = xTextPortionEnumerationAccess->createEnumeration();
        while (xTextPortions->hasMoreElements())
        {
            uno::Reference<beans::XPropertySet> xTextPortion(xTextPortions->nextElement(), uno::UNO_QUERY);
            OUString aTextPortionType;
            xTextPortion->getPropertyValue(UNO_NAME_TEXT_PORTION_TYPE) >>= aTextPortionType;
            if (aTextPortionType != UNO_NAME_TEXT_FIELD)
                continue;

            uno::Reference<lang::XServiceInfo> xTextField;
            xTextPortion->getPropertyValue(UNO_NAME_TEXT_FIELD) >>= xTextField;
            if (!xTextField->supportsService(rServiceName))
                continue;

            OUString aName;
            uno::Reference<beans::XPropertySet> xPropertySet(xTextField, uno::UNO_QUERY);
            xPropertySet->getPropertyValue(UNO_NAME_NAME) >>= aName;
            if (aName == rFieldName)
            {
                xField = uno::Reference<text::XTextField>(xTextField, uno::UNO_QUERY);
                break;
            }
        }
    }

    return xField;
}

/// Search for a field named rFieldName of type rServiceName in xText and return true iff found.
bool lcl_hasField(const uno::Reference<text::XText>& xText, const OUString& rServiceName, const OUString& rFieldName)
{
    return lcl_findClassificationField(xText, rServiceName, rFieldName).is();
}

/// Search for a frame with WATERMARK_NAME in name of type rServiceName in xText. Returns found name in rShapeName.
uno::Reference<drawing::XShape> lcl_getWatermark(const uno::Reference<text::XText>& xText, const OUString& rServiceName, OUString& rShapeName)
{
    uno::Reference<container::XEnumerationAccess> xParagraphEnumerationAccess(xText, uno::UNO_QUERY);
    uno::Reference<container::XEnumeration> xParagraphs = xParagraphEnumerationAccess->createEnumeration();
    while (xParagraphs->hasMoreElements())
    {
        uno::Reference<container::XEnumerationAccess> xTextPortionEnumerationAccess(xParagraphs->nextElement(), uno::UNO_QUERY);
        if (!xTextPortionEnumerationAccess.is())
            continue;

        uno::Reference<container::XEnumeration> xTextPortions = xTextPortionEnumerationAccess->createEnumeration();
        while (xTextPortions->hasMoreElements())
        {
            uno::Reference<beans::XPropertySet> xTextPortion(xTextPortions->nextElement(), uno::UNO_QUERY);
            OUString aTextPortionType;
            xTextPortion->getPropertyValue(UNO_NAME_TEXT_PORTION_TYPE) >>= aTextPortionType;
            if (aTextPortionType != "Frame")
                continue;

            uno::Reference<container::XContentEnumerationAccess> xContentEnumerationAccess(xTextPortion, uno::UNO_QUERY);
            if (!xContentEnumerationAccess.is())
                continue;

            uno::Reference<container::XEnumeration> xEnumeration = xContentEnumerationAccess->createContentEnumeration("com.sun.star.text.TextContent");
            if (!xEnumeration->hasMoreElements())
                continue;

            uno::Reference<lang::XServiceInfo> xWatermark(xEnumeration->nextElement(), uno::UNO_QUERY);
            if (!xWatermark->supportsService(rServiceName))
                continue;

            uno::Reference<container::XNamed> xNamed(xWatermark, uno::UNO_QUERY);

            if (!xNamed->getName().match(WATERMARK_NAME))
                continue;

            rShapeName = xNamed->getName();

            uno::Reference<drawing::XShape> xShape(xWatermark, uno::UNO_QUERY);
            return xShape;
        }
    }

    return uno::Reference<drawing::XShape>();
}

/// Extract the text of the paragraph without any of the fields.
/// TODO: Consider moving to SwTextNode, or extend ModelToViewHelper.
OString lcl_getParagraphBodyText(const uno::Reference<text::XTextContent>& xText)
{
    OUStringBuffer strBuf;
    uno::Reference<container::XEnumerationAccess> xTextPortionEnumerationAccess(xText, uno::UNO_QUERY);
    if (!xTextPortionEnumerationAccess.is())
        return OString();

    uno::Reference<container::XEnumeration> xTextPortions = xTextPortionEnumerationAccess->createEnumeration();
    while (xTextPortions->hasMoreElements())
    {
        uno::Any elem = xTextPortions->nextElement();

        //TODO: Consider including hidden and conditional texts/portions.
        OUString aTextPortionType;
        uno::Reference<beans::XPropertySet> xPropertySet(elem, uno::UNO_QUERY);
        xPropertySet->getPropertyValue(UNO_NAME_TEXT_PORTION_TYPE) >>= aTextPortionType;
        if (aTextPortionType == "Text")
        {
            uno::Reference<text::XTextRange> xTextRange(elem, uno::UNO_QUERY);
            if (xTextRange.is())
                strBuf.append(xTextRange->getString());
        }
    }

    // Cleanup the dummy characters added by fields (which we exclude).
    comphelper::string::remove(strBuf, CH_TXT_ATR_INPUTFIELDSTART);
    comphelper::string::remove(strBuf, CH_TXT_ATR_INPUTFIELDEND);
    comphelper::string::remove(strBuf, CH_TXTATR_BREAKWORD);

    return strBuf.makeStringAndClear().trim().toUtf8();
}

/// Returns RDF (key, value) pair associated with the field, if any.
std::pair<OUString, OUString> lcl_getFieldRDF(const uno::Reference<frame::XModel>& xModel,
                                              const uno::Reference<css::text::XTextField>& xField,
                                              const OUString& rRDFName)
{
    const css::uno::Reference<css::rdf::XResource> xSubject(xField, uno::UNO_QUERY);
    std::map<OUString, OUString> aStatements = SwRDFHelper::getStatements(xModel, MetaNS, xSubject);
    const auto it = aStatements.find(rRDFName);
    return it != aStatements.end() ? std::make_pair(it->first, it->second) : std::make_pair(OUString(), OUString());
}

/// Returns true iff the field in question is paragraph signature.
/// Note: must have associated RDF, since signatures are otherwise just metadata fields.
bool lcl_IsParagraphSignatureField(const uno::Reference<frame::XModel>& xModel,
                                   const uno::Reference<css::text::XTextField>& xField)
{
    return lcl_getFieldRDF(xModel, xField, ParagraphSignatureRDFName).first == ParagraphSignatureRDFName;
}

/// Validate and return validation result and signature field display text.
std::pair<bool, OUString>
lcl_MakeParagraphSignatureFieldText(const uno::Reference<frame::XModel>& xModel,
                                    const uno::Reference<css::text::XTextField>& xField,
                                    const OString& utf8Text)
{
    OUString msg = SwResId(STR_INVALID_SIGNATURE);
    bool valid = false;

    const css::uno::Reference<css::rdf::XResource> xSubject(xField, uno::UNO_QUERY);
    std::map<OUString, OUString> aStatements = SwRDFHelper::getStatements(xModel, MetaNS, xSubject);

    const auto it = aStatements.find(ParagraphSignatureRDFName);
    if (it != aStatements.end())
    {
        const sal_Char* pData = utf8Text.getStr();
        const std::vector<unsigned char> data(pData, pData + utf8Text.getLength());

        OString encSignature;
        if (it->second.convertToString(&encSignature, RTL_TEXTENCODING_UTF8, 0))
        {
            const std::vector<unsigned char> sig(svl::crypto::DecodeHexString(encSignature));
            SignatureInformation aInfo(0);
            valid = svl::crypto::Signing::Verify(data, false, sig, aInfo);
            valid = valid && aInfo.nStatus == css::xml::crypto::SecurityOperationStatus_OPERATION_SUCCEEDED;

            msg = SwResId(STR_SIGNED_BY) + ": " + aInfo.ouSubject + ", ";

            const auto itDate = aStatements.find(ParagraphSignatureDateRDFName);
            msg += (itDate != aStatements.end() ? itDate->second : OUString());

            const auto itUsage = aStatements.find(ParagraphSignatureUsageRDFName);
            msg += (itUsage != aStatements.end() ? (" (" + itUsage->second + "): ") : OUString(": "));

            msg += (valid ? SwResId(STR_VALID) : SwResId(STR_INVALID));
        }
    }

    return std::make_pair(valid, msg);
}

/// Creates and inserts Paragraph Signature Metadata field and creates the RDF entry
uno::Reference<text::XTextField> lcl_InsertParagraphSignature(const uno::Reference<frame::XModel>& xModel,
                                                              const uno::Reference<text::XTextContent>& xParent,
                                                              const OUString& signature,
                                                              const OUString& usage)
{
    uno::Reference<lang::XMultiServiceFactory> xMultiServiceFactory(xModel, uno::UNO_QUERY);
    auto xField = uno::Reference<text::XTextField>(xMultiServiceFactory->createInstance(MetadataFieldServiceName), uno::UNO_QUERY);

    // Add the signature at the end.
    xField->attach(xParent->getAnchor()->getEnd());

    const css::uno::Reference<css::rdf::XResource> xSubject(xField, uno::UNO_QUERY);
    SwRDFHelper::addStatement(xModel, MetaNS, MetaFilename, xSubject, ParagraphSignatureRDFName, signature);
    SwRDFHelper::addStatement(xModel, MetaNS, MetaFilename, xSubject, ParagraphSignatureUsageRDFName, usage);

    // First convert the UTC UNIX timestamp to a tools::DateTime.
    DateTime aDateTime = DateTime::CreateFromUnixTime(time(nullptr));

    // Then convert to a local UNO DateTime.
    aDateTime.ConvertToLocalTime();
    OUStringBuffer rBuffer;
    rBuffer.append((sal_Int32)aDateTime.GetYear());
    rBuffer.append('-');
    if (aDateTime.GetMonth() < 10)
        rBuffer.append('0');
    rBuffer.append((sal_Int32)aDateTime.GetMonth());
    rBuffer.append('-');
    if (aDateTime.GetDay() < 10)
        rBuffer.append('0');
    rBuffer.append((sal_Int32)aDateTime.GetDay());

    SwRDFHelper::addStatement(xModel, MetaNS, MetaFilename, xSubject, ParagraphSignatureDateRDFName, rBuffer.makeStringAndClear());

    return xField;
}

/// Updates the signature field text if changed and returns true only iff updated.
bool lcl_UpdateParagraphSignatureField(SwDoc* pDoc,
                                       const uno::Reference<frame::XModel>& xModel,
                                       const uno::Reference<css::text::XTextField>& xField,
                                       const OString& utf8Text)
{
    // Disable undo to avoid introducing noise when we edit the metadata field.
    const bool isUndoEnabled = pDoc->GetIDocumentUndoRedo().DoesUndo();
    pDoc->GetIDocumentUndoRedo().DoUndo(false);
    comphelper::ScopeGuard const g([pDoc, isUndoEnabled] () {
            pDoc->GetIDocumentUndoRedo().DoUndo(isUndoEnabled);
        });

    const std::pair<bool, OUString> res = lcl_MakeParagraphSignatureFieldText(xModel, xField, utf8Text);
    uno::Reference<css::text::XTextRange> xText(xField, uno::UNO_QUERY);
    const OUString curText = xText->getString();
    if (curText != res.second)
    {
        xText->setString(res.second + " ");
        return true;
    }

    return false;
}

void lcl_RemoveParagraphMetadataField(const uno::Reference<css::text::XTextField>& xField)
{
    uno::Reference<css::text::XTextContent> xFieldTextContent(xField, uno::UNO_QUERY);
    uno::Reference<css::text::XTextRange> xParagraph(xFieldTextContent->getAnchor());
    uno::Reference<css::text::XText> xParagraphText(xParagraph->getText(), uno::UNO_QUERY);
    xParagraphText->removeTextContent(xFieldTextContent);
}

/// Returns true iff the field in question is paragraph classification.
/// Note: must have associated RDF, since classifications are otherwise just metadata fields.
bool lcl_IsParagraphClassificationField(const uno::Reference<frame::XModel>& xModel,
                                        const uno::Reference<css::text::XTextField>& xField,
                                        const OUString& sKey = OUString())
{
    const std::pair<OUString, OUString> rdfPair = lcl_getFieldRDF(xModel, xField, ParagraphClassificationNameRDFName);
    return rdfPair.first == ParagraphClassificationNameRDFName && (sKey.isEmpty() || rdfPair.second == sKey);
}

uno::Reference<text::XTextField> lcl_FindParagraphClassificationField(const uno::Reference<frame::XModel>& xModel,
                                                                      const uno::Reference<text::XTextContent>& xParagraph,
                                                                      const OUString& sKey = OUString())
{
    uno::Reference<text::XTextField> xTextField;

    uno::Reference<container::XEnumerationAccess> xTextPortionEnumerationAccess(xParagraph, uno::UNO_QUERY);
    if (!xTextPortionEnumerationAccess.is())
        return xTextField;

    uno::Reference<container::XEnumeration> xTextPortions = xTextPortionEnumerationAccess->createEnumeration();
    while (xTextPortions->hasMoreElements())
    {
        uno::Reference<beans::XPropertySet> xTextPortion(xTextPortions->nextElement(), uno::UNO_QUERY);
        OUString aTextPortionType;
        xTextPortion->getPropertyValue(UNO_NAME_TEXT_PORTION_TYPE) >>= aTextPortionType;
        if (aTextPortionType != UNO_NAME_TEXT_FIELD)
            continue;

        uno::Reference<lang::XServiceInfo> xServiceInfo;
        xTextPortion->getPropertyValue(UNO_NAME_TEXT_FIELD) >>= xServiceInfo;
        if (!xServiceInfo->supportsService(MetadataFieldServiceName))
            continue;

        uno::Reference<text::XTextField> xField(xServiceInfo, uno::UNO_QUERY);
        if (lcl_IsParagraphClassificationField(xModel, xField, sKey))
        {
            uno::Reference<css::text::XTextRange> xText(xField, uno::UNO_QUERY);
            xTextField = xField;
            break;
        }
    }

    return xTextField;
}

/// Creates and inserts Paragraph Classification Metadata field and creates the RDF entry
uno::Reference<text::XTextField> lcl_InsertParagraphClassification(const uno::Reference<frame::XModel>& xModel,
                                                                   const uno::Reference<text::XTextContent>& xParent)
{
    uno::Reference<lang::XMultiServiceFactory> xMultiServiceFactory(xModel, uno::UNO_QUERY);
    auto xField = uno::Reference<text::XTextField>(xMultiServiceFactory->createInstance(MetadataFieldServiceName), uno::UNO_QUERY);

    // Add the classification at the start.
    xField->attach(xParent->getAnchor()->getStart());
    return xField;
}

/// Updates the paragraph classification field text if changed and returns true only iff updated.
bool lcl_UpdateParagraphClassificationField(SwDoc* pDoc,
                                            const uno::Reference<frame::XModel>& xModel,
                                            const uno::Reference<css::text::XTextField>& xField,
                                            const OUString& sKey,
                                            const OUString& sValue,
                                            const OUString& sDisplayText)
{
    // Disable undo to avoid introducing noise when we edit the metadata field.
    const bool isUndoEnabled = pDoc->GetIDocumentUndoRedo().DoesUndo();
    pDoc->GetIDocumentUndoRedo().DoUndo(false);
    comphelper::ScopeGuard const g([pDoc, isUndoEnabled] () {
            pDoc->GetIDocumentUndoRedo().DoUndo(isUndoEnabled);
        });

    const css::uno::Reference<css::rdf::XResource> xSubject(xField, uno::UNO_QUERY);
    SwRDFHelper::addStatement(xModel, MetaNS, MetaFilename, xSubject, ParagraphClassificationNameRDFName, sKey);
    SwRDFHelper::addStatement(xModel, MetaNS, MetaFilename, xSubject, ParagraphClassificationValueRDFName, sValue);

    uno::Reference<css::text::XTextRange> xText(xField, uno::UNO_QUERY);
    const OUString curDisplayText = xText->getString();
    if (curDisplayText != sDisplayText)
    {
        xText->setString(sDisplayText);
        return true;
    }

    return false;
}

} // anonymous namespace

SwTextFormatColl& SwEditShell::GetDfltTextFormatColl() const
{
    return *GetDoc()->GetDfltTextFormatColl();
}

sal_uInt16 SwEditShell::GetTextFormatCollCount() const
{
    return GetDoc()->GetTextFormatColls()->size();
}

SwTextFormatColl& SwEditShell::GetTextFormatColl(sal_uInt16 nFormatColl) const
{
    return *((*(GetDoc()->GetTextFormatColls()))[nFormatColl]);
}

OUString lcl_getProperty(uno::Reference<beans::XPropertyContainer> const & rxPropertyContainer, const OUString& rName)
{
    try
    {
        uno::Reference<beans::XPropertySet> xPropertySet(rxPropertyContainer, uno::UNO_QUERY);
        return xPropertySet->getPropertyValue(rName).get<OUString>();
    }
    catch (const css::uno::Exception&)
    {
    }

    return OUString();
}

static bool lcl_containsProperty(const uno::Sequence<beans::Property> & rProperties, const OUString& rName)
{
    return std::find_if(rProperties.begin(), rProperties.end(), [&](const beans::Property& rProperty)
    {
        return rProperty.Name == rName;
    }) != rProperties.end();
}

static void lcl_removeAllProperties(uno::Reference<beans::XPropertyContainer> const & rxPropertyContainer)
{
    uno::Reference<beans::XPropertySet> xPropertySet(rxPropertyContainer, uno::UNO_QUERY);
    uno::Sequence<beans::Property> aProperties = xPropertySet->getPropertySetInfo()->getProperties();

    for (const beans::Property& rProperty : aProperties)
    {
        rxPropertyContainer->removeProperty(rProperty.Name);
    }
}

bool addOrInsertDocumentProperty(uno::Reference<beans::XPropertyContainer> const & rxPropertyContainer, OUString const & rsKey, OUString const & rsValue)
{
    uno::Reference<beans::XPropertySet> xPropertySet(rxPropertyContainer, uno::UNO_QUERY);

    try
    {
        if (lcl_containsProperty(xPropertySet->getPropertySetInfo()->getProperties(), rsKey))
            xPropertySet->setPropertyValue(rsKey, uno::makeAny(rsValue));
        else
            rxPropertyContainer->addProperty(rsKey, beans::PropertyAttribute::REMOVABLE, uno::makeAny(rsValue));
    }
    catch (const uno::Exception& /*rException*/)
    {
        return false;
    }
    return true;
}

void insertFieldToDocument(uno::Reference<lang::XMultiServiceFactory> const & rxMultiServiceFactory, uno::Reference<text::XText> const & rxText, OUString const & rsKey)
{
    if (!lcl_hasField(rxText, DocInfoServiceName, rsKey))
    {
        uno::Reference<beans::XPropertySet> xField(rxMultiServiceFactory->createInstance(DocInfoServiceName), uno::UNO_QUERY);
        xField->setPropertyValue(UNO_NAME_NAME, uno::makeAny(rsKey));
        uno::Reference<text::XTextContent> xTextContent(xField, uno::UNO_QUERY);
        rxText->insertTextContent(rxText->getEnd(), xTextContent, false);
    }
}

void SwEditShell::ApplyAdvancedClassification(std::vector<svx::ClassificationResult> const & rResults)
{
    SwDocShell* pDocShell = GetDoc()->GetDocShell();
    if (!pDocShell || !SfxObjectShell::Current())
        return;

    uno::Reference<frame::XModel> xModel = pDocShell->GetBaseModel();
    uno::Reference<style::XStyleFamiliesSupplier> xStyleFamiliesSupplier(xModel, uno::UNO_QUERY);
    uno::Reference<container::XNameAccess> xStyleFamilies(xStyleFamiliesSupplier->getStyleFamilies(), uno::UNO_QUERY);
    uno::Reference<container::XNameAccess> xStyleFamily(xStyleFamilies->getByName("PageStyles"), uno::UNO_QUERY);

    uno::Reference<lang::XMultiServiceFactory> xMultiServiceFactory(xModel, uno::UNO_QUERY);

    uno::Reference<document::XDocumentProperties> xDocumentProperties = SfxObjectShell::Current()->getDocProperties();

    // First, we need to remove the old ones.
    //TODO: we should get this as a param, since we pass it to the dialog.
    const OUString sPolicy = SfxClassificationHelper::policyTypeToString(SfxClassificationHelper::getPolicyType());
    const std::vector<OUString> aUsedPageStyles = lcl_getUsedPageStyles(this);
    for (const OUString& rPageStyleName : aUsedPageStyles)
    {
        uno::Reference<beans::XPropertySet> xPageStyle(xStyleFamily->getByName(rPageStyleName), uno::UNO_QUERY);

        // HEADER
        bool bHeaderIsOn = false;
        xPageStyle->getPropertyValue(UNO_NAME_HEADER_IS_ON) >>= bHeaderIsOn;
        uno::Reference<text::XText> xHeaderText;
        if (bHeaderIsOn)
            xPageStyle->getPropertyValue(UNO_NAME_HEADER_TEXT) >>= xHeaderText;

        // FOOTER
        bool bFooterIsOn = false;
        xPageStyle->getPropertyValue(UNO_NAME_FOOTER_IS_ON) >>= bFooterIsOn;
        uno::Reference<text::XText> xFooterText;
        if (bFooterIsOn)
            xPageStyle->getPropertyValue(UNO_NAME_FOOTER_TEXT) >>= xFooterText;

        sal_Int32 nTextNumber = 1;
        OUString sKey;
        for (svx::ClassificationResult const & rResult : CollectAdvancedClassification())
        {
            sKey = "";
            switch(rResult.meType)
            {
                case svx::ClassificationType::TEXT:
                {
                    sKey = sPolicy + "Marking:Text:" + OUString::number(nTextNumber++);
                }
                break;

                case svx::ClassificationType::CATEGORY:
                {
                    sKey = sPolicy + "BusinessAuthorizationCategory:Name";
                }
                break;

                case svx::ClassificationType::MARKING:
                {
                    sKey = sPolicy + "Extension:Marking";
                }
                break;

                case svx::ClassificationType::INTELLECTUAL_PROPERTY_PART:
                {
                    sKey = sPolicy + "Extension:IntellectualPropertyPart";
                }
                break;

                default:
                break;
            }

            if (!sKey.isEmpty())
            {
                uno::Reference<css::text::XTextField> xField = lcl_findClassificationField(xHeaderText, DocInfoServiceName, sKey);
                if (xField.is())
                {
                    if (xHeaderText.is())
                        xHeaderText->removeTextContent(xField);

                    if (xFooterText.is())
                        xFooterText->removeTextContent(xField);
                }
            }
        }
    }

    // Clear properties
    uno::Reference<beans::XPropertyContainer> xPropertyContainer = xDocumentProperties->getUserDefinedProperties();
    lcl_removeAllProperties(xPropertyContainer);

    SfxClassificationHelper aHelper(xDocumentProperties);

    // Apply properties from the BA policy
    for (svx::ClassificationResult const & rResult : rResults)
    {
        if (rResult.meType == svx::ClassificationType::CATEGORY)
        {
            aHelper.SetBACName(rResult.msString, SfxClassificationHelper::getPolicyType());
        }
    }

    for (const OUString& rPageStyleName : aUsedPageStyles)
    {
        uno::Reference<beans::XPropertySet> xPageStyle(xStyleFamily->getByName(rPageStyleName), uno::UNO_QUERY);

        // HEADER
        bool bHeaderIsOn = false;
        xPageStyle->getPropertyValue(UNO_NAME_HEADER_IS_ON) >>= bHeaderIsOn;
        if (!bHeaderIsOn)
            xPageStyle->setPropertyValue(UNO_NAME_HEADER_IS_ON, uno::makeAny(true));
        uno::Reference<text::XText> xHeaderText;
        xPageStyle->getPropertyValue(UNO_NAME_HEADER_TEXT) >>= xHeaderText;

        // FOOTER
        bool bFooterIsOn = false;
        xPageStyle->getPropertyValue(UNO_NAME_FOOTER_IS_ON) >>= bFooterIsOn;
        if (!bFooterIsOn)
            xPageStyle->setPropertyValue(UNO_NAME_FOOTER_IS_ON, uno::makeAny(true));
        uno::Reference<text::XText> xFooterText;
        xPageStyle->getPropertyValue(UNO_NAME_FOOTER_TEXT) >>= xFooterText;

        sal_Int32 nTextNumber = 1;

        for (svx::ClassificationResult const & rResult : rResults)
        {
            switch(rResult.meType)
            {
                case svx::ClassificationType::TEXT:
                {
                    OUString sKey = sPolicy + "Marking:Text:" + OUString::number(nTextNumber);
                    nTextNumber++;

                    addOrInsertDocumentProperty(xPropertyContainer, sKey, rResult.msString);
                    insertFieldToDocument(xMultiServiceFactory, xHeaderText, sKey);
                    insertFieldToDocument(xMultiServiceFactory, xFooterText, sKey);
                }
                break;

                case svx::ClassificationType::CATEGORY:
                {
                    OUString sKey = sPolicy + "BusinessAuthorizationCategory:Name";
                    insertFieldToDocument(xMultiServiceFactory, xHeaderText, sKey);
                    insertFieldToDocument(xMultiServiceFactory, xFooterText, sKey);
                }
                break;

                case svx::ClassificationType::MARKING:
                {
                    OUString sKey = sPolicy + "Extension:Marking";
                    addOrInsertDocumentProperty(xPropertyContainer, sKey, rResult.msString);
                    insertFieldToDocument(xMultiServiceFactory, xHeaderText, sKey);
                    insertFieldToDocument(xMultiServiceFactory, xFooterText, sKey);
                }
                break;

                case svx::ClassificationType::INTELLECTUAL_PROPERTY_PART:
                {
                    OUString sKey = sPolicy + "Extension:IntellectualPropertyPart";
                    addOrInsertDocumentProperty(xPropertyContainer, sKey, rResult.msString);
                    insertFieldToDocument(xMultiServiceFactory, xHeaderText, sKey);
                    insertFieldToDocument(xMultiServiceFactory, xFooterText, sKey);
                }
                break;

                default:
                break;
            }
        }
    }
}

std::vector<svx::ClassificationResult> SwEditShell::CollectAdvancedClassification()
{
    std::vector<svx::ClassificationResult> aResult;

    SwDocShell* pDocShell = GetDoc()->GetDocShell();
    if (!pDocShell || !SfxObjectShell::Current())
        return aResult;

    uno::Reference<frame::XModel> xModel = pDocShell->GetBaseModel();
    uno::Reference<style::XStyleFamiliesSupplier> xStyleFamiliesSupplier(xModel, uno::UNO_QUERY);
    uno::Reference<container::XNameAccess> xStyleFamilies(xStyleFamiliesSupplier->getStyleFamilies(), uno::UNO_QUERY);
    uno::Reference<container::XNameAccess> xStyleFamily(xStyleFamilies->getByName("PageStyles"), uno::UNO_QUERY);

    std::vector<OUString> aPageStyles = lcl_getUsedPageStyles(this);
    OUString aPageStyleString = aPageStyles.back();
    uno::Reference<beans::XPropertySet> xPageStyle(xStyleFamily->getByName(aPageStyleString), uno::UNO_QUERY);

    bool bHeaderIsOn = false;
    xPageStyle->getPropertyValue(UNO_NAME_HEADER_IS_ON) >>= bHeaderIsOn;
    if (!bHeaderIsOn)
        return aResult;

    uno::Reference<text::XText> xHeaderText;
    xPageStyle->getPropertyValue(UNO_NAME_HEADER_TEXT) >>= xHeaderText;

    uno::Reference<container::XEnumerationAccess> xParagraphEnumerationAccess(xHeaderText, uno::UNO_QUERY);
    uno::Reference<container::XEnumeration> xParagraphs = xParagraphEnumerationAccess->createEnumeration();

    uno::Reference<document::XDocumentProperties> xDocumentProperties = SfxObjectShell::Current()->getDocProperties();
    uno::Reference<beans::XPropertyContainer> xPropertyContainer = xDocumentProperties->getUserDefinedProperties();

    OUString sPolicy = SfxClassificationHelper::policyTypeToString(SfxClassificationHelper::getPolicyType());
    sal_Int32 nParagraph = 1;

    while (xParagraphs->hasMoreElements())
    {
        uno::Reference<container::XEnumerationAccess> xTextPortionEnumerationAccess(xParagraphs->nextElement(), uno::UNO_QUERY);
        uno::Reference<container::XEnumeration> xTextPortions = xTextPortionEnumerationAccess->createEnumeration();
        while (xTextPortions->hasMoreElements())
        {
            uno::Reference<beans::XPropertySet> xTextPortion(xTextPortions->nextElement(), uno::UNO_QUERY);
            OUString aTextPortionType;
            xTextPortion->getPropertyValue(UNO_NAME_TEXT_PORTION_TYPE) >>= aTextPortionType;
            if (aTextPortionType != UNO_NAME_TEXT_FIELD)
                continue;

            uno::Reference<lang::XServiceInfo> xTextField;
            xTextPortion->getPropertyValue(UNO_NAME_TEXT_FIELD) >>= xTextField;
            if (!xTextField->supportsService(DocInfoServiceName))
                continue;

            OUString aName;
            uno::Reference<beans::XPropertySet> xPropertySet(xTextField, uno::UNO_QUERY);
            xPropertySet->getPropertyValue(UNO_NAME_NAME) >>= aName;
            const OUString sBlank("");
            if (aName.startsWith(sPolicy + "Marking:Text:"))
            {
                const OUString aValue = lcl_getProperty(xPropertyContainer, aName);
                if (!aValue.isEmpty())
                    aResult.push_back({ svx::ClassificationType::TEXT, aValue, sBlank, nParagraph });
            }
            else if (aName.startsWith(sPolicy + "BusinessAuthorizationCategory:Name"))
            {
                const OUString aValue = lcl_getProperty(xPropertyContainer, aName);
                if (!aValue.isEmpty())
                    aResult.push_back({ svx::ClassificationType::CATEGORY, aValue, sBlank, nParagraph });
            }
            else if (aName.startsWith(sPolicy + "Extension:Marking"))
            {
                const OUString aValue = lcl_getProperty(xPropertyContainer, aName);
                if (!aValue.isEmpty())
                    aResult.push_back({ svx::ClassificationType::MARKING, aValue, sBlank, nParagraph });
            }
            else if (aName.startsWith(sPolicy + "Extension:IntellectualPropertyPart"))
            {
                const OUString aValue = lcl_getProperty(xPropertyContainer, aName);
                if (!aValue.isEmpty())
                    aResult.push_back({ svx::ClassificationType::INTELLECTUAL_PROPERTY_PART, aValue, sBlank, nParagraph });
            }
        }
        ++nParagraph;
    }

    return aResult;
}

void SwEditShell::SetClassification(const OUString& rName, SfxClassificationPolicyType eType)
{
    SwDocShell* pDocShell = GetDoc()->GetDocShell();
    if (!pDocShell)
        return;

    SfxClassificationHelper aHelper(pDocShell->getDocProperties());

    const bool bHadWatermark = !aHelper.GetDocumentWatermark().isEmpty();

    // This updates the infobar as well.
    aHelper.SetBACName(rName, eType);

    bool bHeaderIsNeeded = aHelper.HasDocumentHeader();
    bool bFooterIsNeeded = aHelper.HasDocumentFooter();
    OUString aWatermark = aHelper.GetDocumentWatermark();
    bool bWatermarkIsNeeded = !aWatermark.isEmpty();

    if (!bHeaderIsNeeded && !bFooterIsNeeded && !bWatermarkIsNeeded && !bHadWatermark)
        return;

    uno::Reference<frame::XModel> xModel = pDocShell->GetBaseModel();
    uno::Reference<style::XStyleFamiliesSupplier> xStyleFamiliesSupplier(xModel, uno::UNO_QUERY);
    uno::Reference<container::XNameAccess> xStyleFamilies(xStyleFamiliesSupplier->getStyleFamilies(), uno::UNO_QUERY);
    uno::Reference<container::XNameAccess> xStyleFamily(xStyleFamilies->getByName("PageStyles"), uno::UNO_QUERY);
    uno::Sequence<OUString> aStyles = xStyleFamily->getElementNames();

    for (const OUString& rPageStyleName : aStyles)
    {
        uno::Reference<beans::XPropertySet> xPageStyle(xStyleFamily->getByName(rPageStyleName), uno::UNO_QUERY);
        uno::Reference<lang::XMultiServiceFactory> xMultiServiceFactory(xModel, uno::UNO_QUERY);

        if (bHeaderIsNeeded || bWatermarkIsNeeded || bHadWatermark)
        {
            // If the header is off, turn it on.
            bool bHeaderIsOn = false;
            xPageStyle->getPropertyValue(UNO_NAME_HEADER_IS_ON) >>= bHeaderIsOn;
            if (!bHeaderIsOn)
                xPageStyle->setPropertyValue(UNO_NAME_HEADER_IS_ON, uno::makeAny(true));

            // If the header already contains a document header field, no need to do anything.
            uno::Reference<text::XText> xHeaderText;
            xPageStyle->getPropertyValue(UNO_NAME_HEADER_TEXT) >>= xHeaderText;

            if (bHeaderIsNeeded)
            {
                if (!lcl_hasField(xHeaderText, DocInfoServiceName, SfxClassificationHelper::PROP_PREFIX_INTELLECTUALPROPERTY() + SfxClassificationHelper::PROP_DOCHEADER()))
                {
                    // Append a field to the end of the header text.
                    uno::Reference<beans::XPropertySet> xField(xMultiServiceFactory->createInstance(DocInfoServiceName), uno::UNO_QUERY);
                    xField->setPropertyValue(UNO_NAME_NAME, uno::makeAny(SfxClassificationHelper::PROP_PREFIX_INTELLECTUALPROPERTY() + SfxClassificationHelper::PROP_DOCHEADER()));
                    uno::Reference<text::XTextContent> xTextContent(xField, uno::UNO_QUERY);
                    xHeaderText->insertTextContent(xHeaderText->getEnd(), xTextContent, /*bAbsorb=*/false);
                }
            }

            SfxWatermarkItem aWatermarkItem;
            aWatermarkItem.SetText(aWatermark);
            SetWatermark(aWatermarkItem);
        }

        if (bFooterIsNeeded)
        {
            // If the footer is off, turn it on.
            bool bFooterIsOn = false;
            xPageStyle->getPropertyValue(UNO_NAME_FOOTER_IS_ON) >>= bFooterIsOn;
            if (!bFooterIsOn)
                xPageStyle->setPropertyValue(UNO_NAME_FOOTER_IS_ON, uno::makeAny(true));

            // If the footer already contains a document header field, no need to do anything.
            uno::Reference<text::XText> xFooterText;
            xPageStyle->getPropertyValue(UNO_NAME_FOOTER_TEXT) >>= xFooterText;
            static OUString sFooter = SfxClassificationHelper::PROP_PREFIX_INTELLECTUALPROPERTY() + SfxClassificationHelper::PROP_DOCFOOTER();
            if (!lcl_hasField(xFooterText, DocInfoServiceName, sFooter))
            {
                // Append a field to the end of the footer text.
                uno::Reference<beans::XPropertySet> xField(xMultiServiceFactory->createInstance(DocInfoServiceName), uno::UNO_QUERY);
                xField->setPropertyValue(UNO_NAME_NAME, uno::makeAny(sFooter));
                uno::Reference<text::XTextContent> xTextContent(xField, uno::UNO_QUERY);
                xFooterText->insertTextContent(xFooterText->getEnd(), xTextContent, /*bAbsorb=*/false);
            }
        }
    }
}

void SwEditShell::ApplyParagraphClassification(std::vector<svx::ClassificationResult> aResults)
{
    SwDocShell* pDocShell = GetDoc()->GetDocShell();
    if (!pDocShell)
        return;

    SwTextNode* pNode = GetCursor()->Start()->nNode.GetNode().GetTextNode();
    if (pNode == nullptr)
        return;

    uno::Reference<text::XTextContent> xParent = SwXParagraph::CreateXParagraph(*pNode->GetDoc(), pNode);
    uno::Reference<frame::XModel> xModel = pDocShell->GetBaseModel();
    uno::Reference<lang::XMultiServiceFactory> xMultiServiceFactory(xModel, uno::UNO_QUERY);

    const OUString sPolicy = SfxClassificationHelper::policyTypeToString(SfxClassificationHelper::getPolicyType());

    // Prevent recursive validation since this is triggered on node updates, which we do below.
    const bool bOldValidationFlag = SetParagraphSignatureValidation(false);
    comphelper::ScopeGuard const g([this, bOldValidationFlag] () {
            SetParagraphSignatureValidation(bOldValidationFlag);
        });

    // Remove all paragraph classification fields.
    for (;;)
    {
        uno::Reference<text::XTextField> xTextField = lcl_FindParagraphClassificationField(xModel, xParent);
        if (!xTextField.is())
            break;
        lcl_RemoveParagraphMetadataField(xTextField);
    }

    // Since we always insert at the start of the paragraph,
    // need to insert in reverse order.
    std::reverse(aResults.begin(), aResults.end());
    sal_Int32 nTextNumber = 1;
    for (size_t nIndex = 0; nIndex < aResults.size(); ++nIndex)
    {
        const svx::ClassificationResult& rResult = aResults[nIndex];
        const bool isLast = nIndex == 0;
        const bool isFirst = nIndex == aResults.size() - 1;
        OUString sKey;
        switch(rResult.meType)
        {
            case svx::ClassificationType::TEXT:
            {
                sKey = sPolicy + "Marking:Text:" + OUString::number(nTextNumber++);
            }
            break;

            case svx::ClassificationType::CATEGORY:
            {
                sKey = sPolicy + "BusinessAuthorizationCategory:Name";
            }
            break;

            case svx::ClassificationType::MARKING:
            {
                sKey = sPolicy + "Extension:Marking";
            }
            break;

            case svx::ClassificationType::INTELLECTUAL_PROPERTY_PART:
            {
                sKey = sPolicy + "Extension:IntellectualPropertyPart";
            }
            break;

            default:
            break;
        }

        uno::Reference<text::XTextField> xTextField = lcl_InsertParagraphClassification(xModel, xParent);
        OUString sDisplayText = (isFirst ? ("(" + rResult.msAbbreviatedString) : rResult.msAbbreviatedString);
        if (isLast)
            sDisplayText += ")";
        lcl_UpdateParagraphClassificationField(GetDoc(), xModel, xTextField, sKey, rResult.msString, sDisplayText);
    }
}

std::vector<svx::ClassificationResult> SwEditShell::CollectParagraphClassification()
{
    std::vector<svx::ClassificationResult> aResult;

    SwDocShell* pDocShell = GetDoc()->GetDocShell();
    if (!pDocShell)
        return aResult;

    SwTextNode* pNode = GetCursor()->Start()->nNode.GetNode().GetTextNode();
    if (pNode == nullptr)
        return aResult;

    uno::Reference<text::XTextContent> xParent = SwXParagraph::CreateXParagraph(*pNode->GetDoc(), pNode);
    uno::Reference<frame::XModel> xModel = pDocShell->GetBaseModel();
    uno::Reference<lang::XMultiServiceFactory> xMultiServiceFactory(xModel, uno::UNO_QUERY);

    uno::Reference<container::XEnumerationAccess> xTextPortionEnumerationAccess(xParent, uno::UNO_QUERY);
    if (!xTextPortionEnumerationAccess.is())
        return aResult;

    uno::Reference<container::XEnumeration> xTextPortions = xTextPortionEnumerationAccess->createEnumeration();

    const OUString sPolicy = SfxClassificationHelper::policyTypeToString(SfxClassificationHelper::getPolicyType());
    const sal_Int32 nParagraph = 1;

    while (xTextPortions->hasMoreElements())
    {
        uno::Reference<beans::XPropertySet> xTextPortion(xTextPortions->nextElement(), uno::UNO_QUERY);
        OUString aTextPortionType;
        xTextPortion->getPropertyValue(UNO_NAME_TEXT_PORTION_TYPE) >>= aTextPortionType;
        if (aTextPortionType != UNO_NAME_TEXT_FIELD)
            continue;

        uno::Reference<lang::XServiceInfo> xField;
        xTextPortion->getPropertyValue(UNO_NAME_TEXT_FIELD) >>= xField;
        if (!xField->supportsService(MetadataFieldServiceName))
            continue;

        uno::Reference<text::XTextField> xTextField(xField, uno::UNO_QUERY);
        const std::pair<OUString, OUString> rdfNamePair = lcl_getFieldRDF(xModel, xTextField, ParagraphClassificationNameRDFName);
        const std::pair<OUString, OUString> rdfValuePair = lcl_getFieldRDF(xModel, xTextField, ParagraphClassificationValueRDFName);

        uno::Reference<text::XTextRange> xTextRange(xField, uno::UNO_QUERY);
        const OUString aName = rdfNamePair.second;
        const OUString aValue = rdfValuePair.second;
        const OUString sBlank("");
        if (aName.startsWith(sPolicy + "Marking:Text:"))
        {
            aResult.push_back({ svx::ClassificationType::TEXT, aValue, sBlank, nParagraph });
        }
        else if (aName.startsWith(sPolicy + "BusinessAuthorizationCategory:Name"))
        {
            aResult.push_back({ svx::ClassificationType::CATEGORY, aValue, sBlank, nParagraph });
        }
        else if (aName.startsWith(sPolicy + "Extension:Marking"))
        {
            aResult.push_back({ svx::ClassificationType::MARKING, aValue, sBlank, nParagraph });
        }
        else if (aName.startsWith(sPolicy + "Extension:IntellectualPropertyPart"))
        {
            aResult.push_back({ svx::ClassificationType::INTELLECTUAL_PROPERTY_PART, xTextRange->getString(), sBlank, nParagraph });
        }
    }

    return aResult;
}

sal_Int16 lcl_GetAngle(const drawing::HomogenMatrix3& rMatrix)
{
    basegfx::B2DHomMatrix aTransformation;
    basegfx::B2DTuple aScale;
    basegfx::B2DTuple aTranslate;
    double fRotate = 0;
    double fShear = 0;

    aTransformation.set(0, 0, rMatrix.Line1.Column1);
    aTransformation.set(0, 1, rMatrix.Line1.Column2);
    aTransformation.set(0, 2, rMatrix.Line1.Column3);
    aTransformation.set(1, 0, rMatrix.Line2.Column1);
    aTransformation.set(1, 1, rMatrix.Line2.Column2);
    aTransformation.set(1, 2, rMatrix.Line2.Column3);
    aTransformation.set(2, 0, rMatrix.Line3.Column1);
    aTransformation.set(2, 1, rMatrix.Line3.Column2);
    aTransformation.set(2, 2, rMatrix.Line3.Column3);

    aTransformation.decompose(aScale, aTranslate, fRotate, fShear);
    sal_Int16 nDeg = round(basegfx::rad2deg(fRotate));
    return nDeg < 0 ? round(nDeg) * -1 : round(360.0 - nDeg);
}

SfxWatermarkItem SwEditShell::GetWatermark()
{
    SwDocShell* pDocShell = GetDoc()->GetDocShell();
    if (!pDocShell)
        return SfxWatermarkItem();

    uno::Reference<frame::XModel> xModel = pDocShell->GetBaseModel();
    uno::Reference<style::XStyleFamiliesSupplier> xStyleFamiliesSupplier(xModel, uno::UNO_QUERY);
    uno::Reference<container::XNameAccess> xStyleFamilies(xStyleFamiliesSupplier->getStyleFamilies(), uno::UNO_QUERY);
    uno::Reference<container::XNameAccess> xStyleFamily(xStyleFamilies->getByName("PageStyles"), uno::UNO_QUERY);
    std::vector<OUString> aUsedPageStyles = lcl_getUsedPageStyles(this);
    for (const OUString& rPageStyleName : aUsedPageStyles)
    {
        uno::Reference<beans::XPropertySet> xPageStyle(xStyleFamily->getByName(rPageStyleName), uno::UNO_QUERY);
        uno::Reference<lang::XMultiServiceFactory> xMultiServiceFactory(xModel, uno::UNO_QUERY);

        bool bHeaderIsOn = false;
        xPageStyle->getPropertyValue(UNO_NAME_HEADER_IS_ON) >>= bHeaderIsOn;
        if (!bHeaderIsOn)
            return SfxWatermarkItem();

        uno::Reference<text::XText> xHeaderText;
        xPageStyle->getPropertyValue(UNO_NAME_HEADER_TEXT) >>= xHeaderText;

        OUString aShapeServiceName = "com.sun.star.drawing.CustomShape";
        OUString sWatermark = "";
        uno::Reference<drawing::XShape> xWatermark = lcl_getWatermark(xHeaderText, aShapeServiceName, sWatermark);

        if (xWatermark.is())
        {
            SfxWatermarkItem aItem;
            uno::Reference<text::XTextRange> xTextRange(xWatermark, uno::UNO_QUERY);
            uno::Reference<beans::XPropertySet> xPropertySet(xWatermark, uno::UNO_QUERY);
            sal_uInt32 nColor;
            sal_Int16 nTransparency;
            OUString aFont;
            drawing::HomogenMatrix3 aMatrix;

            aItem.SetText(xTextRange->getString());

            if (xPropertySet->getPropertyValue(UNO_NAME_CHAR_FONT_NAME) >>= aFont)
                aItem.SetFont(aFont);
            if (xPropertySet->getPropertyValue(UNO_NAME_FILLCOLOR) >>= nColor)
                aItem.SetColor(nColor);
            if (xPropertySet->getPropertyValue("Transformation") >>= aMatrix)
                aItem.SetAngle(lcl_GetAngle(aMatrix));
            if (xPropertySet->getPropertyValue(UNO_NAME_FILL_TRANSPARENCE) >>= nTransparency)
                aItem.SetTransparency(nTransparency);

            return aItem;
        }
    }
    return SfxWatermarkItem();
}

void lcl_placeWatermarkInHeader(const SfxWatermarkItem& rWatermark,
                            const uno::Reference<frame::XModel>& xModel,
                            const uno::Reference<beans::XPropertySet>& xPageStyle,
                            const uno::Reference<text::XText>& xHeaderText)
{
    uno::Reference<lang::XMultiServiceFactory> xMultiServiceFactory(xModel, uno::UNO_QUERY);
    OUString aShapeServiceName = "com.sun.star.drawing.CustomShape";
    OUString sWatermark = WATERMARK_NAME;
    uno::Reference<drawing::XShape> xWatermark = lcl_getWatermark(xHeaderText, aShapeServiceName, sWatermark);

    bool bDeleteWatermark = rWatermark.GetText().isEmpty();
    if (xWatermark.is())
    {
        drawing::HomogenMatrix3 aMatrix;
        sal_uInt32 nColor = 0xc0c0c0;
        sal_Int16 nTransparency = 50;
        sal_Int16 nAngle = 45;
        OUString aFont = "";

        uno::Reference<beans::XPropertySet> xPropertySet(xWatermark, uno::UNO_QUERY);
        xPropertySet->getPropertyValue(UNO_NAME_CHAR_FONT_NAME) >>= aFont;
        xPropertySet->getPropertyValue(UNO_NAME_FILLCOLOR) >>= nColor;
        xPropertySet->getPropertyValue(UNO_NAME_FILL_TRANSPARENCE) >>= nTransparency;
        xPropertySet->getPropertyValue("Transformation") >>= aMatrix;
        nAngle = lcl_GetAngle(aMatrix);

        // If the header already contains a watermark, see if it its text is up to date.
        uno::Reference<text::XTextRange> xTextRange(xWatermark, uno::UNO_QUERY);
        if (xTextRange->getString() != rWatermark.GetText()
            || aFont != rWatermark.GetFont()
            || nColor != rWatermark.GetColor()
            || nAngle != rWatermark.GetAngle()
            || nTransparency != rWatermark.GetTransparency()
            || bDeleteWatermark)
        {
            // No: delete it and we'll insert a replacement.
            uno::Reference<lang::XComponent> xComponent(xWatermark, uno::UNO_QUERY);
            xComponent->dispose();
            xWatermark.clear();
        }
    }

    if (!xWatermark.is() && !bDeleteWatermark)
    {
        OUString sFont = rWatermark.GetFont();
        sal_Int16 nAngle = rWatermark.GetAngle();
        sal_Int16 nTransparency = rWatermark.GetTransparency();
        sal_uInt32 nColor = rWatermark.GetColor();

        // Calc the ratio.
        double fRatio = 0;
        OutputDevice* pOut = Application::GetDefaultDevice();
        vcl::Font aFont(pOut->GetFont());
        aFont.SetFamilyName(sFont);
        auto nTextWidth = pOut->GetTextWidth(rWatermark.GetText());
        if (nTextWidth)
        {
            fRatio = aFont.GetFontSize().Height();
            fRatio /= nTextWidth;
        }

        // Calc the size.
        sal_Int32 nWidth = 0;
        awt::Size aSize;
        xPageStyle->getPropertyValue(UNO_NAME_SIZE) >>= aSize;
        if (aSize.Width < aSize.Height)
        {
            // Portrait.
            sal_Int32 nLeftMargin = 0;
            xPageStyle->getPropertyValue(UNO_NAME_LEFT_MARGIN) >>= nLeftMargin;
            sal_Int32 nRightMargin = 0;
            xPageStyle->getPropertyValue(UNO_NAME_RIGHT_MARGIN) >>= nRightMargin;
            nWidth = aSize.Width - nLeftMargin - nRightMargin;
        }
        else
        {
            // Landscape.
            sal_Int32 nTopMargin = 0;
            xPageStyle->getPropertyValue(UNO_NAME_TOP_MARGIN) >>= nTopMargin;
            sal_Int32 nBottomMargin = 0;
            xPageStyle->getPropertyValue(UNO_NAME_BOTTOM_MARGIN) >>= nBottomMargin;
            nWidth = aSize.Height - nTopMargin - nBottomMargin;
        }
        sal_Int32 nHeight = nWidth * fRatio;

        // Create and insert the shape.
        uno::Reference<drawing::XShape> xShape(xMultiServiceFactory->createInstance(aShapeServiceName), uno::UNO_QUERY);
        basegfx::B2DHomMatrix aTransformation;
        aTransformation.identity();
        aTransformation.scale(nWidth, nHeight);
        aTransformation.rotate(F_PI180 * -1 * nAngle);
        drawing::HomogenMatrix3 aMatrix;
        aMatrix.Line1.Column1 = aTransformation.get(0, 0);
        aMatrix.Line1.Column2 = aTransformation.get(0, 1);
        aMatrix.Line1.Column3 = aTransformation.get(0, 2);
        aMatrix.Line2.Column1 = aTransformation.get(1, 0);
        aMatrix.Line2.Column2 = aTransformation.get(1, 1);
        aMatrix.Line2.Column3 = aTransformation.get(1, 2);
        aMatrix.Line3.Column1 = aTransformation.get(2, 0);
        aMatrix.Line3.Column2 = aTransformation.get(2, 1);
        aMatrix.Line3.Column3 = aTransformation.get(2, 2);
        uno::Reference<beans::XPropertySet> xPropertySet(xShape, uno::UNO_QUERY);
        xPropertySet->setPropertyValue(UNO_NAME_ANCHOR_TYPE, uno::makeAny(text::TextContentAnchorType_AT_CHARACTER));
        uno::Reference<text::XTextContent> xTextContent(xShape, uno::UNO_QUERY);
        xHeaderText->insertTextContent(xHeaderText->getEnd(), xTextContent, false);

        // The remaining properties have to be set after the shape is inserted: do that in one batch to avoid flickering.
        uno::Reference<document::XActionLockable> xLockable(xShape, uno::UNO_QUERY);
        xLockable->addActionLock();
        xPropertySet->setPropertyValue(UNO_NAME_FILLCOLOR, uno::makeAny(static_cast<sal_Int32>(nColor)));
        xPropertySet->setPropertyValue(UNO_NAME_FILLSTYLE, uno::makeAny(drawing::FillStyle_SOLID));
        xPropertySet->setPropertyValue(UNO_NAME_FILL_TRANSPARENCE, uno::makeAny(nTransparency));
        xPropertySet->setPropertyValue(UNO_NAME_HORI_ORIENT_RELATION, uno::makeAny(static_cast<sal_Int16>(text::RelOrientation::PAGE_PRINT_AREA)));
        xPropertySet->setPropertyValue(UNO_NAME_LINESTYLE, uno::makeAny(drawing::LineStyle_NONE));
        xPropertySet->setPropertyValue(UNO_NAME_OPAQUE, uno::makeAny(false));
        xPropertySet->setPropertyValue(UNO_NAME_TEXT_AUTOGROWHEIGHT, uno::makeAny(false));
        xPropertySet->setPropertyValue(UNO_NAME_TEXT_AUTOGROWWIDTH, uno::makeAny(false));
        xPropertySet->setPropertyValue(UNO_NAME_TEXT_MINFRAMEHEIGHT, uno::makeAny(nHeight));
        xPropertySet->setPropertyValue(UNO_NAME_TEXT_MINFRAMEWIDTH, uno::makeAny(nWidth));
        xPropertySet->setPropertyValue(UNO_NAME_TEXT_WRAP, uno::makeAny(text::WrapTextMode_THROUGH));
        xPropertySet->setPropertyValue(UNO_NAME_VERT_ORIENT_RELATION, uno::makeAny(static_cast<sal_Int16>(text::RelOrientation::PAGE_PRINT_AREA)));
        xPropertySet->setPropertyValue(UNO_NAME_CHAR_FONT_NAME, uno::makeAny(sFont));
        xPropertySet->setPropertyValue("Transformation", uno::makeAny(aMatrix));
        xPropertySet->setPropertyValue(UNO_NAME_HORI_ORIENT, uno::makeAny(static_cast<sal_Int16>(text::HoriOrientation::CENTER)));
        xPropertySet->setPropertyValue(UNO_NAME_VERT_ORIENT, uno::makeAny(static_cast<sal_Int16>(text::VertOrientation::CENTER)));

        uno::Reference<text::XTextRange> xTextRange(xShape, uno::UNO_QUERY);
        xTextRange->setString(rWatermark.GetText());

        uno::Reference<drawing::XEnhancedCustomShapeDefaulter> xDefaulter(xShape, uno::UNO_QUERY);
        xDefaulter->createCustomShapeDefaults("fontwork-plain-text");

        auto aGeomPropSeq = xPropertySet->getPropertyValue("CustomShapeGeometry").get< uno::Sequence<beans::PropertyValue> >();
        auto aGeomPropVec = comphelper::sequenceToContainer< std::vector<beans::PropertyValue> >(aGeomPropSeq);
        uno::Sequence<beans::PropertyValue> aPropertyValues(comphelper::InitPropertySequence(
        {
            {"TextPath", uno::makeAny(true)},
        }));
        auto it = std::find_if(aGeomPropVec.begin(), aGeomPropVec.end(), [](const beans::PropertyValue& rValue)
        {
            return rValue.Name == "TextPath";
        });
        if (it == aGeomPropVec.end())
            aGeomPropVec.push_back(comphelper::makePropertyValue("TextPath", aPropertyValues));
        else
            it->Value <<= aPropertyValues;
        xPropertySet->setPropertyValue("CustomShapeGeometry", uno::makeAny(comphelper::containerToSequence(aGeomPropVec)));

        // tdf#108494, tdf#109313 the header height was switched to height of a watermark
        // and shape was moved to the lower part of a page, force position update
        xPropertySet->getPropertyValue("Transformation") >>= aMatrix;
        xPropertySet->setPropertyValue("Transformation", uno::makeAny(aMatrix));

        uno::Reference<container::XNamed> xNamed(xShape, uno::UNO_QUERY);
        xNamed->setName(sWatermark);
        xLockable->removeActionLock();
    }
}

void SwEditShell::SetWatermark(const SfxWatermarkItem& rWatermark)
{
    SwDocShell* pDocShell = GetDoc()->GetDocShell();
    if (!pDocShell)
        return;

    uno::Reference<frame::XModel> xModel = pDocShell->GetBaseModel();
    uno::Reference<style::XStyleFamiliesSupplier> xStyleFamiliesSupplier(xModel, uno::UNO_QUERY);
    uno::Reference<container::XNameAccess> xStyleFamilies(xStyleFamiliesSupplier->getStyleFamilies(), uno::UNO_QUERY);
    uno::Reference<container::XNameAccess> xStyleFamily(xStyleFamilies->getByName("PageStyles"), uno::UNO_QUERY);
    uno::Sequence<OUString> aStyles = xStyleFamily->getElementNames();

    for (const OUString& rPageStyleName : aStyles)
    {
        uno::Reference<beans::XPropertySet> xPageStyle(xStyleFamily->getByName(rPageStyleName), uno::UNO_QUERY);

        // If the header is off, turn it on.
        bool bHeaderIsOn = false;
        xPageStyle->getPropertyValue(UNO_NAME_HEADER_IS_ON) >>= bHeaderIsOn;
        if (!bHeaderIsOn)
            xPageStyle->setPropertyValue(UNO_NAME_HEADER_IS_ON, uno::makeAny(true));

        // backup header height
        bool bDynamicHeight = true;
        sal_Int32 nOldValue;
        xPageStyle->getPropertyValue(UNO_NAME_HEADER_HEIGHT) >>= nOldValue;
        xPageStyle->getPropertyValue(UNO_NAME_HEADER_IS_DYNAMIC_HEIGHT) >>= bDynamicHeight;
        xPageStyle->setPropertyValue(UNO_NAME_HEADER_IS_DYNAMIC_HEIGHT, uno::Any(false));

        // If the header already contains a document header field, no need to do anything.
        uno::Reference<text::XText> xHeaderText;
        uno::Reference<text::XText> xHeaderTextFirst;
        uno::Reference<text::XText> xHeaderTextLeft;
        uno::Reference<text::XText> xHeaderTextRight;

        xPageStyle->getPropertyValue(UNO_NAME_HEADER_TEXT) >>= xHeaderText;
        lcl_placeWatermarkInHeader(rWatermark, xModel, xPageStyle, xHeaderText);

        xPageStyle->getPropertyValue(UNO_NAME_HEADER_TEXT_FIRST) >>= xHeaderTextFirst;
        lcl_placeWatermarkInHeader(rWatermark, xModel, xPageStyle, xHeaderTextFirst);

        xPageStyle->getPropertyValue(UNO_NAME_HEADER_TEXT_LEFT) >>= xHeaderTextLeft;
        lcl_placeWatermarkInHeader(rWatermark, xModel, xPageStyle, xHeaderTextLeft);

        xPageStyle->getPropertyValue(UNO_NAME_HEADER_TEXT_RIGHT) >>= xHeaderTextRight;
        lcl_placeWatermarkInHeader(rWatermark, xModel, xPageStyle, xHeaderTextRight);

        // tdf#108494 the header height was switched to height of a watermark
        // and shape was moved to the lower part of a page
        xPageStyle->setPropertyValue(UNO_NAME_HEADER_HEIGHT, uno::makeAny((sal_Int32)11));
        xPageStyle->setPropertyValue(UNO_NAME_HEADER_HEIGHT, uno::makeAny(nOldValue));
        xPageStyle->setPropertyValue(UNO_NAME_HEADER_IS_DYNAMIC_HEIGHT, uno::Any(bDynamicHeight));
    }
}

SwUndoParagraphSigning::SwUndoParagraphSigning(const SwPosition& rPos,
                                               const uno::Reference<text::XTextField>& xField,
                                               const uno::Reference<text::XTextContent>& xParent,
                                               const bool bRemove)
  : SwUndo(SwUndoId::PARA_SIGN_ADD, rPos.GetDoc()),
    m_pDoc(rPos.GetDoc()),
    m_xField(xField),
    m_xParent(xParent),
    m_bRemove(bRemove)
{
    // Save the metadata and field content to undo/redo.
    uno::Reference<frame::XModel> xModel = m_pDoc->GetDocShell()->GetBaseModel();
    const css::uno::Reference<css::rdf::XResource> xSubject(m_xField, uno::UNO_QUERY);
    std::map<OUString, OUString> aStatements = SwRDFHelper::getStatements(xModel, MetaNS, xSubject);
    const auto it = aStatements.find(ParagraphSignatureRDFName);
    if (it != aStatements.end())
        m_signature = it->second;

    const auto it2 = aStatements.find(ParagraphSignatureUsageRDFName);
    if (it2 != aStatements.end())
        m_usage = it2->second;

    uno::Reference<css::text::XTextRange> xText(m_xField, uno::UNO_QUERY);
    m_display = xText->getString();
}

void SwUndoParagraphSigning::UndoImpl(::sw::UndoRedoContext&)
{
    if (m_bRemove)
        Remove();
    else
        Insert();
}

void SwUndoParagraphSigning::RedoImpl(::sw::UndoRedoContext&)
{
    if (m_bRemove)
        Insert();
    else
        Remove();
}

void SwUndoParagraphSigning::RepeatImpl(::sw::RepeatContext&)
{
}

void SwUndoParagraphSigning::Insert()
{
    // Disable undo to avoid introducing noise when we edit the metadata field.
    const bool isUndoEnabled = m_pDoc->GetIDocumentUndoRedo().DoesUndo();
    m_pDoc->GetIDocumentUndoRedo().DoUndo(false);

    // Prevent validation since this will trigger a premature validation
    // upon inserting, but before setting the metadata.
    SwEditShell* pEditSh = m_pDoc->GetEditShell();
    const bool bOldValidationFlag = pEditSh->SetParagraphSignatureValidation(false);
    comphelper::ScopeGuard const g([&] () {
            pEditSh->SetParagraphSignatureValidation(bOldValidationFlag);
            m_pDoc->GetIDocumentUndoRedo().DoUndo(isUndoEnabled);
        });

    m_xField = lcl_InsertParagraphSignature(m_pDoc->GetDocShell()->GetBaseModel(), m_xParent, m_signature, m_usage);

    uno::Reference<css::text::XTextRange> xText(m_xField, uno::UNO_QUERY);
    xText->setString(m_display);
}

void SwUndoParagraphSigning::Remove()
{
    // Disable undo to avoid introducing noise when we edit the metadata field.
    const bool isUndoEnabled = m_pDoc->GetIDocumentUndoRedo().DoesUndo();
    m_pDoc->GetIDocumentUndoRedo().DoUndo(false);

    // Prevent validation since this will trigger a premature validation
    // upon removing.
    SwEditShell* pEditSh = m_pDoc->GetEditShell();
    const bool bOldValidationFlag = pEditSh->SetParagraphSignatureValidation(false);
    comphelper::ScopeGuard const g([&] () {
            pEditSh->SetParagraphSignatureValidation(bOldValidationFlag);
            m_pDoc->GetIDocumentUndoRedo().DoUndo(isUndoEnabled);
        });

    lcl_RemoveParagraphMetadataField(m_xField);
}

void SwEditShell::SignParagraph()
{
    SwDocShell* pDocShell = GetDoc()->GetDocShell();
    if (!pDocShell)
        return;
    const SwPosition* pPosStart = GetCursor()->Start();
    if (!pPosStart)
        return;
    SwTextNode* pNode = pPosStart->nNode.GetNode().GetTextNode();
    if (!pNode)
        return;

    // 1. Get the text (without fields).
    const uno::Reference<text::XTextContent> xParent = SwXParagraph::CreateXParagraph(*pNode->GetDoc(), pNode);
    const OString utf8Text = lcl_getParagraphBodyText(xParent);
    if (utf8Text.isEmpty())
        return;

    // 2. Get certificate.
    uno::Reference<security::XDocumentDigitalSignatures> xSigner(
        security::DocumentDigitalSignatures::createWithVersion(
            comphelper::getProcessComponentContext(), "1.2" ) );

    uno::Sequence<css::beans::PropertyValue> aProperties;
    uno::Reference<security::XCertificate> xCertificate = xSigner->chooseCertificateWithProps(aProperties);
    if (!xCertificate.is())
        return;

    // 3. Sign it.
    svl::crypto::Signing signing(xCertificate);
    signing.AddDataRange(utf8Text.getStr(), utf8Text.getLength());
    OStringBuffer sigBuf;
    if (!signing.Sign(sigBuf))
        return;

    const OUString signature = OStringToOUString(sigBuf.makeStringAndClear(), RTL_TEXTENCODING_UTF8, 0);

    std::vector<css::beans::PropertyValue> vec = comphelper::sequenceToContainer<std::vector<css::beans::PropertyValue>>(aProperties);
    auto it = std::find_if(vec.begin(), vec.end(), [](const beans::PropertyValue& rValue)
                                                    {
                                                        return rValue.Name == "Usage";
                                                    });

    OUString aUsage;
    if (it != vec.end())
        it->Value >>= aUsage;

    // 4. Add metadata
    // Prevent validation since this will trigger a premature validation
    // upon inserting, but before setting the metadata.
    const bool bOldValidationFlag = SetParagraphSignatureValidation(false);
    comphelper::ScopeGuard const g([this, bOldValidationFlag] () {
            SetParagraphSignatureValidation(bOldValidationFlag);
        });

    GetDoc()->GetIDocumentUndoRedo().StartUndo(SwUndoId::PARA_SIGN_ADD, nullptr);

    const uno::Reference<frame::XModel> xModel = pDocShell->GetBaseModel();
    uno::Reference<css::text::XTextField> xField = lcl_InsertParagraphSignature(xModel, xParent, signature, aUsage);

    lcl_UpdateParagraphSignatureField(GetDoc(), xModel, xField, utf8Text);

    SwUndoParagraphSigning* pUndo = new SwUndoParagraphSigning(SwPosition(*pNode), xField, xParent, true);
    GetDoc()->GetIDocumentUndoRedo().AppendUndo(pUndo);

    GetDoc()->GetIDocumentUndoRedo().EndUndo(SwUndoId::PARA_SIGN_ADD, nullptr);
}

void SwEditShell::ValidateParagraphSignatures(bool updateDontRemove)
{
    SwDocShell* pDocShell = GetDoc()->GetDocShell();
    if (!pDocShell || !IsParagraphSignatureValidationEnabled())
        return;

    SwPaM* pPaM = GetCursor();
    const SwPosition* pPosStart = pPaM->Start();
    SwTextNode* pNode = pPosStart->nNode.GetNode().GetTextNode();
    if (!pNode)
        return;

    // 2. For each signature field, update it.
    const uno::Reference<text::XTextContent> xParent = SwXParagraph::CreateXParagraph(*pNode->GetDoc(), pNode);
    uno::Reference<container::XEnumerationAccess> xTextPortionEnumerationAccess(xParent, uno::UNO_QUERY);
    if (!xTextPortionEnumerationAccess.is())
        return;

    uno::Reference<frame::XModel> xModel = pDocShell->GetBaseModel();
    uno::Reference<container::XEnumeration> xTextPortions = xTextPortionEnumerationAccess->createEnumeration();

    // Prevent recursive validation since this is triggered on node updates, which we do below.
    const bool bOldValidationFlag = SetParagraphSignatureValidation(false);
    comphelper::ScopeGuard const g([this, bOldValidationFlag] () {
            SetParagraphSignatureValidation(bOldValidationFlag);
        });

    // 2. Get the text (without fields).
    const OString utf8Text = lcl_getParagraphBodyText(xParent);
    if (utf8Text.isEmpty())
        return;

    while (xTextPortions->hasMoreElements())
    {
        uno::Reference<beans::XPropertySet> xTextPortion(xTextPortions->nextElement(), uno::UNO_QUERY);
        OUString aTextPortionType;
        xTextPortion->getPropertyValue(UNO_NAME_TEXT_PORTION_TYPE) >>= aTextPortionType;
        if (aTextPortionType != UNO_NAME_TEXT_FIELD)
            continue;

        uno::Reference<lang::XServiceInfo> xTextField;
        xTextPortion->getPropertyValue(UNO_NAME_TEXT_FIELD) >>= xTextField;
        if (!xTextField->supportsService(MetadataFieldServiceName))
            continue;

        uno::Reference<text::XTextField> xField(xTextField, uno::UNO_QUERY);
        if (!lcl_IsParagraphSignatureField(xModel, xField))
        {
            continue;
        }

        if (updateDontRemove)
        {
            lcl_UpdateParagraphSignatureField(GetDoc(), xModel, xField, utf8Text);
        }
        else if (!lcl_MakeParagraphSignatureFieldText(xModel, xField, utf8Text).first)
        {
            GetDoc()->GetIDocumentUndoRedo().StartUndo(SwUndoId::PARA_SIGN_ADD, nullptr);
            SwUndoParagraphSigning* pUndo = new SwUndoParagraphSigning(SwPosition(*pNode), xField, xParent, false);
            GetDoc()->GetIDocumentUndoRedo().AppendUndo(pUndo);
            lcl_RemoveParagraphMetadataField(xField);
            GetDoc()->GetIDocumentUndoRedo().EndUndo(SwUndoId::PARA_SIGN_ADD, nullptr);
        }
    }
}

uno::Reference<text::XTextField> lcl_GetParagraphMetadataFieldAtIndex(const SwDocShell* pDocSh, SwTextNode* pNode, const sal_uLong index)
{
    uno::Reference<text::XTextField> xTextField;
    if (pNode != nullptr && pDocSh != nullptr)
    {
        SwTextAttr* pAttr = pNode->GetTextAttrAt(index, RES_TXTATR_METAFIELD);
        SwTextMeta* pTextMeta = static_txtattr_cast<SwTextMeta*>(pAttr);
        if (pTextMeta != nullptr)
        {
            SwFormatMeta& rFormatMeta(static_cast<SwFormatMeta&>(pTextMeta->GetAttr()));
            if (::sw::Meta* pMeta = rFormatMeta.GetMeta())
            {
                const css::uno::Reference<css::rdf::XResource> xSubject(pMeta->MakeUnoObject(), uno::UNO_QUERY);
                uno::Reference<frame::XModel> xModel = pDocSh->GetBaseModel();
                const std::map<OUString, OUString> aStatements = SwRDFHelper::getStatements(xModel, MetaNS, xSubject);
                if (aStatements.find(ParagraphSignatureRDFName) != aStatements.end() ||
                    aStatements.find(ParagraphClassificationNameRDFName) != aStatements.end())
                {
                    xTextField = uno::Reference<text::XTextField>(xSubject, uno::UNO_QUERY);
                }
            }
        }
    }

    return xTextField;
}

bool SwEditShell::IsCursorInParagraphMetadataField() const
{
    SwTextNode* pNode = GetCursor()->Start()->nNode.GetNode().GetTextNode();
    const sal_uLong index = GetCursor()->Start()->nContent.GetIndex();
    uno::Reference<text::XTextField> xField = lcl_GetParagraphMetadataFieldAtIndex(GetDoc()->GetDocShell(), pNode, index);
    return xField.is();
}

bool SwEditShell::RemoveParagraphMetadataFieldAtCursor(const bool bBackspaceNotDel)
{
    SwTextNode* pNode = GetCursor()->Start()->nNode.GetNode().GetTextNode();
    sal_uLong index = GetCursor()->Start()->nContent.GetIndex();
    uno::Reference<text::XTextField> xField = lcl_GetParagraphMetadataFieldAtIndex(GetDoc()->GetDocShell(), pNode, index);
    if (!xField.is())
    {
        // Try moving the cursor to see if we're _facing_ a metafield or not,
        // as opposed to being within one.
        if (bBackspaceNotDel)
            index--; // Backspace moves left
        else
            index++; // Delete moves right

        xField = lcl_GetParagraphMetadataFieldAtIndex(GetDoc()->GetDocShell(), pNode, index);
    }

    if (xField.is())
    {
        lcl_RemoveParagraphMetadataField(xField);
        return true;
    }

    return false;
}

OUString lcl_GetParagraphClassification(const uno::Reference<frame::XModel>& xModel, const uno::Reference<text::XTextContent>& xParagraph)
{
    const OUString sPolicy = SfxClassificationHelper::policyTypeToString(SfxClassificationHelper::getPolicyType());
    uno::Reference<text::XTextField> xTextField = lcl_FindParagraphClassificationField(xModel, xParagraph, sPolicy + "BusinessAuthorizationCategory:Name");
    if (xTextField.is())
    {
        const std::pair<OUString, OUString> rdfValuePair = lcl_getFieldRDF(xModel, xTextField, ParagraphClassificationValueRDFName);
        return rdfValuePair.second;
    }

    return OUString();
}

OUString lcl_GetHighestClassificationParagraphClass(SwPaM* pCursor)
{
    OUString sHighestClass;

    SwTextNode* pNode = pCursor->Start()->nNode.GetNode().GetTextNode();
    if (pNode == nullptr)
        return sHighestClass;

    SwDocShell* pDocShell = pNode->GetDoc()->GetDocShell();
    if (!pDocShell)
        return sHighestClass;

    SfxClassificationHelper aHelper(pDocShell->getDocProperties());

    uno::Reference<frame::XModel> xModel = pDocShell->GetBaseModel();
    const uno::Reference< text::XTextDocument > xDoc(xModel, uno::UNO_QUERY);
    uno::Reference<text::XText> xParent = xDoc->getText();

    uno::Reference<container::XEnumerationAccess> xParagraphEnumerationAccess(xParent, uno::UNO_QUERY);
    uno::Reference<container::XEnumeration> xParagraphs = xParagraphEnumerationAccess->createEnumeration();
    while (xParagraphs->hasMoreElements())
    {
        uno::Reference<text::XTextContent> xParagraph(xParagraphs->nextElement(), uno::UNO_QUERY);
        sHighestClass = aHelper.GetHigherClass(sHighestClass, lcl_GetParagraphClassification(xModel, xParagraph));
    }

    return sHighestClass;
}

void SwEditShell::ClassifyDocPerHighestParagraphClass()
{
    SwDocShell* pDocShell = GetDoc()->GetDocShell();
    if (!pDocShell)
        return;

    SfxClassificationHelper aHelper(pDocShell->getDocProperties());

    const OUString sHighestParaClass = lcl_GetHighestClassificationParagraphClass(GetCursor());

    std::vector<svx::ClassificationResult> results = CollectAdvancedClassification();
    for (const svx::ClassificationResult& rResult : results)
    {
        switch (rResult.meType)
        {
        case svx::ClassificationType::CATEGORY:
        {
            const OUString sHighestClass = aHelper.GetHigherClass(sHighestParaClass, rResult.msString);
            const auto eType = SfxClassificationHelper::stringToPolicyType(sHighestClass);
            SetClassification(sHighestClass, eType);
        }
        break;
        default:
        break;
        }
    }

    if (results.empty())
    {
        const auto eType = SfxClassificationHelper::stringToPolicyType(sHighestParaClass);
        SetClassification(sHighestParaClass, eType);
    }
}

// #i62675#
void SwEditShell::SetTextFormatColl(SwTextFormatColl *pFormat,
                                const bool bResetListAttrs)
{
    SwTextFormatColl *pLocal = pFormat? pFormat: (*GetDoc()->GetTextFormatColls())[0];
    StartAllAction();

    SwRewriter aRewriter;
    aRewriter.AddRule(UndoArg1, pLocal->GetName());

    GetDoc()->GetIDocumentUndoRedo().StartUndo(SwUndoId::SETFMTCOLL, &aRewriter);
    for(SwPaM& rPaM : GetCursor()->GetRingContainer())
    {

        if ( !rPaM.HasReadonlySel( GetViewOptions()->IsFormView() ) )
        {
            // Change the paragraph style to pLocal and remove all direct paragraph formatting.
            GetDoc()->SetTextFormatColl( rPaM, pLocal, true, bResetListAttrs );

            // If there are hints on the nodes which cover the whole node, then remove those, too.
            SwPaM aPaM(*rPaM.Start(), *rPaM.End());
            if (SwTextNode* pEndTextNode = aPaM.End()->nNode.GetNode().GetTextNode())
            {
                aPaM.Start()->nContent = 0;
                aPaM.End()->nContent = pEndTextNode->GetText().getLength();
            }
            GetDoc()->RstTextAttrs(aPaM, /*bInclRefToxMark=*/false, /*bExactRange=*/true);
        }

    }
    GetDoc()->GetIDocumentUndoRedo().EndUndo(SwUndoId::SETFMTCOLL, &aRewriter);
    EndAllAction();
}

SwTextFormatColl* SwEditShell::MakeTextFormatColl(const OUString& rFormatCollName,
        SwTextFormatColl* pParent)
{
    SwTextFormatColl *pColl;
    if ( pParent == nullptr )
        pParent = &GetTextFormatColl(0);
    if (  (pColl=GetDoc()->MakeTextFormatColl(rFormatCollName, pParent)) == nullptr )
    {
        OSL_FAIL( "MakeTextFormatColl failed" );
    }
    return pColl;

}

void SwEditShell::FillByEx(SwTextFormatColl* pColl)
{
    SwPaM * pCursor = GetCursor();
    SwContentNode * pCnt = pCursor->GetContentNode();
    const SfxItemSet* pSet = pCnt->GetpSwAttrSet();
    if( pSet )
    {
        // JP 05.10.98: Special treatment if one of the attribues Break/PageDesc/NumRule(auto) is
        //      in the ItemSet. Otherwise there will be too much or wrong processing (NumRules!)
        //      Bug 57568

        // Do NOT copy AutoNumRules into the template
        const SfxPoolItem* pItem;
        const SwNumRule* pRule = nullptr;
        if( SfxItemState::SET == pSet->GetItemState( RES_BREAK, false ) ||
            SfxItemState::SET == pSet->GetItemState( RES_PAGEDESC,false ) ||
            ( SfxItemState::SET == pSet->GetItemState( RES_PARATR_NUMRULE,
                false, &pItem ) && nullptr != (pRule = GetDoc()->FindNumRulePtr(
                static_cast<const SwNumRuleItem*>(pItem)->GetValue() )) &&
                pRule && pRule->IsAutoRule() )
            )
        {
            SfxItemSet aSet( *pSet );
            aSet.ClearItem( RES_BREAK );
            aSet.ClearItem( RES_PAGEDESC );

            if( pRule || (SfxItemState::SET == pSet->GetItemState( RES_PARATR_NUMRULE,
                false, &pItem ) && nullptr != (pRule = GetDoc()->FindNumRulePtr(
                static_cast<const SwNumRuleItem*>(pItem)->GetValue() )) &&
                pRule && pRule->IsAutoRule() ))
                aSet.ClearItem( RES_PARATR_NUMRULE );

            if( aSet.Count() )
                GetDoc()->ChgFormat(*pColl, aSet );
        }
        else
            GetDoc()->ChgFormat(*pColl, *pSet );
    }
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
