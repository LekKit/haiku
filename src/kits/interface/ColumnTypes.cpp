/*******************************************************************************
/
/	File:			ColumnTypes.h
/
/   Description:    Experimental classes that implement particular column/field
/					data types for use in BColumnListView.
/
/	Copyright 2000+, Be Incorporated, All Rights Reserved
/
*******************************************************************************/

#include "ColumnTypes.h"

#include <View.h>

#include <parsedate.h>
#include <stdio.h>


#define kTEXT_MARGIN	8


const int64 kKB_SIZE = 1024;
const int64 kMB_SIZE = 1048576;
const int64 kGB_SIZE = 1073741824;
const int64 kTB_SIZE = kGB_SIZE * kKB_SIZE;

const char* kSIZE_FORMATS[] = {
	"%.2f %s",
	"%.1f %s",
	"%.f %s",
	"%.f%s",
	0
};


BTitledColumn::BTitledColumn(const char* title, float width, float minWidth,
	float maxWidth, alignment align)
	:
	BColumn(width, minWidth, maxWidth, align),
	fTitle(title)
{
	font_height fh;

	be_plain_font->GetHeight(&fh);
	fFontHeight = fh.descent + fh.leading;
}


void
BTitledColumn::DrawTitle(BRect rect, BView* parent)
{
	float width = rect.Width() - (2 * kTEXT_MARGIN);
	BString out_string(fTitle);

	parent->TruncateString(&out_string, B_TRUNCATE_END, width + 2);
	DrawString(out_string.String(), parent, rect);
}


void
BTitledColumn::GetColumnName(BString* into) const
{
	*into = fTitle;
}


void
BTitledColumn::DrawString(const char* string, BView* parent, BRect rect)
{
	float width = rect.Width() - (2 * kTEXT_MARGIN);
	float y;
	BFont font;
	font_height	finfo;

	parent->GetFont(&font);
	font.GetHeight(&finfo);
	y = rect.top + finfo.ascent
		+ (rect.Height() - ceilf(finfo.ascent + finfo.descent)) / 2.0f;

	switch (Alignment()) {
		default:
		case B_ALIGN_LEFT:
			parent->MovePenTo(rect.left + kTEXT_MARGIN, y);
			break;

		case B_ALIGN_CENTER:
			parent->MovePenTo(rect.left + kTEXT_MARGIN
				+ ((width - font.StringWidth(string)) / 2), y);
			break;

		case B_ALIGN_RIGHT:
			parent->MovePenTo(rect.right - kTEXT_MARGIN
				- font.StringWidth(string), y);
			break;
	}

	parent->DrawString(string);
}


void
BTitledColumn::SetTitle(const char* title)
{
	fTitle.SetTo(title);
}


void
BTitledColumn::Title(BString* forTitle) const
{
	if (forTitle)
		forTitle->SetTo(fTitle.String());
}


float
BTitledColumn::FontHeight() const
{
	return fFontHeight;
}


float
BTitledColumn::GetPreferredWidth(BField *_field, BView* parent) const
{
	return parent->StringWidth(fTitle.String()) + 2 * kTEXT_MARGIN;
}


// #pragma mark - BStringField


BStringField::BStringField(const char* string)
	:
	fWidth(0),
	fString(string),
	fClippedString(string)
{
}


void
BStringField::SetString(const char* val)
{
	fString = val;
	fClippedString = "";
	fWidth = 0;
}


const char*
BStringField::String() const
{
	return fString.String();
}


void
BStringField::SetWidth(float width)
{
	fWidth = width;
}


float
BStringField::Width()
{
	return fWidth;
}


void
BStringField::SetClippedString(const char* val)
{
	fClippedString = val;
}


bool
BStringField::HasClippedString() const
{
	return !fClippedString.IsEmpty();
}


const char*
BStringField::ClippedString()
{
	return fClippedString.String();
}


// #pragma mark - BStringColumn


BStringColumn::BStringColumn(const char* title, float width, float minWidth,
	float maxWidth, uint32 truncate, alignment align)
	:
	BTitledColumn(title, width, minWidth, maxWidth, align),
	fTruncate(truncate)
{
}


void
BStringColumn::DrawField(BField* _field, BRect rect, BView* parent)
{
	float width = rect.Width() - (2 * kTEXT_MARGIN);
	BStringField* field = static_cast<BStringField*>(_field);
	float fieldWidth = field->Width();
	bool updateNeeded = width != fieldWidth;

	if (updateNeeded) {
		BString out_string(field->String());
		float preferredWidth = parent->StringWidth(out_string.String());
		if (width < preferredWidth) {
			parent->TruncateString(&out_string, fTruncate, width + 2);
			field->SetClippedString(out_string.String());
		} else
			field->SetClippedString("");
		field->SetWidth(width);
	}

	DrawString(field->HasClippedString()
		? field->ClippedString()
		: field->String(), parent, rect);
}


float
BStringColumn::GetPreferredWidth(BField *_field, BView* parent) const
{
	BStringField* field = static_cast<BStringField*>(_field);
	return parent->StringWidth(field->String()) + 2 * kTEXT_MARGIN;
}


int
BStringColumn::CompareFields(BField* field1, BField* field2)
{
	return ICompare(((BStringField*)field1)->String(),
		(((BStringField*)field2)->String()));
}


bool
BStringColumn::AcceptsField(const BField *field) const
{
	return static_cast<bool>(dynamic_cast<const BStringField*>(field));
}


// #pragma mark - BDateField


BDateField::BDateField(time_t* time)
	:
	fTime(*localtime(time)),
	fUnixTime(*time),
	fSeconds(0),
	fClippedString(""),
	fWidth(0)
{
	fSeconds = mktime(&fTime);
}


void
BDateField::SetWidth(float width)
{
	fWidth = width;
}


float
BDateField::Width()
{
	return fWidth;
}


void
BDateField::SetClippedString(const char* string)
{
	fClippedString = string;
}


const char*
BDateField::ClippedString()
{
	return fClippedString.String();
}


time_t
BDateField::Seconds()
{
	return fSeconds;
}


time_t
BDateField::UnixTime()
{
	return fUnixTime;
}


// #pragma mark - BDateColumn


BDateColumn::BDateColumn(const char* title, float width, float minWidth,
	float maxWidth, alignment align)
	:
	BTitledColumn(title, width, minWidth, maxWidth, align),
	fTitle(title)
{
}


const char *kTIME_FORMATS[] = {
	"%A, %B %d %Y, %I:%M:%S %p",	// Monday, July 09 1997, 05:08:15 PM
	"%a, %b %d %Y, %I:%M:%S %p",	// Mon, Jul 09 1997, 05:08:15 PM
	"%a, %b %d %Y, %I:%M %p",		// Mon, Jul 09 1997, 05:08 PM
	"%b %d %Y, %I:%M %p",			// Jul 09 1997, 05:08 PM
	"%m/%d/%y, %I:%M %p",			// 07/09/97, 05:08 PM
	"%m/%d/%y",						// 07/09/97
	NULL
};


void
BDateColumn::DrawField(BField* _field, BRect rect, BView* parent)
{
	float width = rect.Width() - (2 * kTEXT_MARGIN);
	BDateField* field = (BDateField*)_field;

	if (field->Width() != rect.Width()) {
		char dateString[256];
		time_t currentTime = field->UnixTime();
		tm time_data;
		BFont font;

		parent->GetFont(&font);
		localtime_r(&currentTime, &time_data);

		for (int32 index = 0; ; index++) {
			if (!kTIME_FORMATS[index])
				break;

			strftime(dateString, 256, kTIME_FORMATS[index], &time_data);
			if (font.StringWidth(dateString) <= width)
				break;
		}

		if (font.StringWidth(dateString) > width) {
			BString out_string(dateString);

			parent->TruncateString(&out_string, B_TRUNCATE_MIDDLE, width + 2);
			strcpy(dateString, out_string.String());
		}
		field->SetClippedString(dateString);
		field->SetWidth(width);
	}

	DrawString(field->ClippedString(), parent, rect);
}


int
BDateColumn::CompareFields(BField* field1, BField* field2)
{
	return((BDateField*)field1)->Seconds() - ((BDateField*)field2)->Seconds();
}


// #pragma mark - BSizeField


BSizeField::BSizeField(off_t size)
	:
	fSize(size)
{
}


void
BSizeField::SetSize(off_t size)
{
	fSize = size;
}


off_t
BSizeField::Size()
{
	return fSize;
}


// #pragma mark - BSizeColumn


BSizeColumn::BSizeColumn(const char* title, float width, float minWidth,
	float maxWidth, alignment align)
	:
	BTitledColumn(title, width, minWidth, maxWidth, align)
{
}


void
BSizeColumn::DrawField(BField* _field, BRect rect, BView* parent)
{
	float width = rect.Width() - (2 * kTEXT_MARGIN);
	BFont font;
	BString string;
	off_t size = ((BSizeField*)_field)->Size();

	parent->GetFont(&font);
	if (size < kKB_SIZE) {
		string.SetToFormat("%" B_PRId64 " bytes", size);
		if (font.StringWidth(string) > width)
			string.SetToFormat("%" B_PRId64 " B", size);
	} else {
		const char*	suffix;
		float float_value;
		if (size >= kTB_SIZE) {
			suffix = "TB";
			float_value = (float)size / kTB_SIZE;
		} else if (size >= kGB_SIZE) {
			suffix = "GB";
			float_value = (float)size / kGB_SIZE;
		} else if (size >= kMB_SIZE) {
			suffix = "MB";
			float_value = (float)size / kMB_SIZE;
		} else {
			suffix = "KB";
			float_value = (float)size / kKB_SIZE;
		}

		for (int32 index = 0; ; index++) {
			if (!kSIZE_FORMATS[index])
				break;

			string.SetToFormat(kSIZE_FORMATS[index], float_value, suffix);
			// strip off an insignificant zero so we don't get readings
			// such as 1.00
			char *period = 0;
			char *tmp (NULL);
			for (tmp = (char*)string.String(); *tmp; tmp++) {
				if (*tmp == '.')
					period = tmp;
			}
			if (period && period[1] && period[2] == '0') {
				// move the rest of the string over the insignificant zero
				for (tmp = &period[2]; *tmp; tmp++)
					*tmp = tmp[1];
			}
			if (font.StringWidth(string) <= width)
				break;
		}
	}

	parent->TruncateString(&string, B_TRUNCATE_MIDDLE, width + 2);
	DrawString(string.String(), parent, rect);
}


int
BSizeColumn::CompareFields(BField* field1, BField* field2)
{
	off_t diff = ((BSizeField*)field1)->Size() - ((BSizeField*)field2)->Size();
	if (diff > 0)
		return 1;
	if (diff < 0)
		return -1;
	return 0;
}


// #pragma mark - BIntegerField


BIntegerField::BIntegerField(int32 number)
	:
	fInteger(number)
{
}


void
BIntegerField::SetValue(int32 value)
{
	fInteger = value;
}


int32
BIntegerField::Value()
{
	return fInteger;
}


// #pragma mark - BIntegerColumn


BIntegerColumn::BIntegerColumn(const char* title, float width, float minWidth,
	float maxWidth, alignment align)
	:
	BTitledColumn(title, width, minWidth, maxWidth, align)
{
}


void
BIntegerColumn::DrawField(BField *field, BRect rect, BView* parent)
{
	float width = rect.Width() - (2 * kTEXT_MARGIN);
	BString string;

	string.SetToFormat("%d", (int)((BIntegerField*)field)->Value());

	parent->TruncateString(&string, B_TRUNCATE_MIDDLE, width + 2);
	DrawString(string.String(), parent, rect);
}


int
BIntegerColumn::CompareFields(BField *field1, BField *field2)
{
	return (((BIntegerField*)field1)->Value() - ((BIntegerField*)field2)->Value());
}


// #pragma mark - GraphColumn


GraphColumn::GraphColumn(const char* name, float width, float minWidth,
	float maxWidth, alignment align)
	:
	BIntegerColumn(name, width, minWidth, maxWidth, align)
{
}


void
GraphColumn::DrawField(BField* field, BRect rect, BView* parent)
{
	int number = ((BIntegerField*)field)->Value();

	if (number > 100)
		number = 100;
	else if (number < 0)
		number = 0;

	BRect graphRect(rect);
	graphRect.InsetBy(5, 3);
	parent->StrokeRect(graphRect);
	if (number > 0) {
		graphRect.InsetBy(1, 1);
		float value = graphRect.Width() * (float)number / 100;
		graphRect.right = graphRect.left + value;
		parent->SetHighColor(0, 0, 190);
		parent->FillRect(graphRect);
	}

	parent->SetDrawingMode(B_OP_INVERT);
	parent->SetHighColor(128, 128, 128);
	char numberString[256];
	sprintf(numberString, "%d%%", number);

	float width = be_plain_font->StringWidth(numberString);
	parent->MovePenTo(rect.left + rect.Width() / 2 - width / 2, rect.bottom - FontHeight());
	parent->DrawString(numberString);
}


// #pragma mark - BBitmapField


BBitmapField::BBitmapField(BBitmap* bitmap)
	:
	fBitmap(bitmap)
{
}


const BBitmap*
BBitmapField::Bitmap()
{
	return fBitmap;
}


void
BBitmapField::SetBitmap(BBitmap* bitmap)
{
	fBitmap = bitmap;
}


// #pragma mark - BBitmapColumn


BBitmapColumn::BBitmapColumn(const char* title, float width, float minWidth,
	float maxWidth, alignment align)
	:
	BTitledColumn(title, width, minWidth, maxWidth, align)
{
}


void
BBitmapColumn::DrawField(BField* field, BRect rect, BView* parent)
{
	BBitmapField* bitmapField = static_cast<BBitmapField*>(field);
	const BBitmap* bitmap = bitmapField->Bitmap();

	if (bitmap != NULL) {
		float x = 0.0;
		BRect r = bitmap->Bounds();
		float y = rect.top + ((rect.Height() - r.Height()) / 2);

		switch (Alignment()) {
			default:
			case B_ALIGN_LEFT:
				x = rect.left + kTEXT_MARGIN;
				break;

			case B_ALIGN_CENTER:
				x = rect.left + ((rect.Width() - r.Width()) / 2);
				break;

			case B_ALIGN_RIGHT:
				x = rect.right - kTEXT_MARGIN - r.Width();
				break;
		}
		// setup drawing mode according to bitmap color space,
		// restore previous mode after drawing
		drawing_mode oldMode = parent->DrawingMode();
		if (bitmap->ColorSpace() == B_RGBA32
			|| bitmap->ColorSpace() == B_RGBA32_BIG) {
			parent->SetDrawingMode(B_OP_ALPHA);
			parent->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
		} else {
			parent->SetDrawingMode(B_OP_OVER);
		}

		parent->DrawBitmap(bitmap, BPoint(x, y));

		parent->SetDrawingMode(oldMode);
	}
}


int
BBitmapColumn::CompareFields(BField* /*field1*/, BField* /*field2*/)
{
	// Comparing bitmaps doesn't really make sense...
	return 0;
}


bool
BBitmapColumn::AcceptsField(const BField *field) const
{
	return static_cast<bool>(dynamic_cast<const BBitmapField*>(field));
}
