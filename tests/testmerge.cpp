// SPDX-License-Identifier: GPL-2.0
#include "testmerge.h"
#include "core/dive.h"
#include "core/divelist.h"
#include "core/file.h"
#include <QTextStream>

void TestMerge::initTestCase()
{
	/* we need to manually tell that the resource exists, because we are using it as library. */
	Q_INIT_RESOURCE(subsurface);
}

void TestMerge::cleanup()
{
	clear_dive_file_data();
}

void TestMerge::testMergeEmpty()
{
	/*
	 * check that we correctly merge mixed cylinder dives
	 */
	struct dive_table table = { 0 };
	struct trip_table trips = { 0 };
	QCOMPARE(parse_file(SUBSURFACE_TEST_DATA "/dives/test47.xml", &table, &trips), 0);
	add_imported_dives(&table, &trips, false, false, true);
	QCOMPARE(parse_file(SUBSURFACE_TEST_DATA "/dives/test48.xml", &table, &trips), 0);
	add_imported_dives(&table, &trips, false, false, true);
	QCOMPARE(save_dives("./testmerge47+48.ssrf"), 0);
	QFile org(SUBSURFACE_TEST_DATA "/dives/test47+48.xml");
	org.open(QFile::ReadOnly);
	QFile out("./testmerge47+48.ssrf");
	out.open(QFile::ReadOnly);
	QTextStream orgS(&org);
	QTextStream outS(&out);
	QStringList readin = orgS.readAll().split("\n");
	QStringList written = outS.readAll().split("\n");
	while (readin.size() && written.size()) {
		QCOMPARE(written.takeFirst().trimmed(), readin.takeFirst().trimmed());
	}
}

void TestMerge::testMergeBackwards()
{
	/*
	 * check that we correctly merge mixed cylinder dives
	 */
	struct dive_table table = { 0 };
	struct trip_table trips = { 0 };
	QCOMPARE(parse_file(SUBSURFACE_TEST_DATA "/dives/test48.xml", &table, &trips), 0);
	add_imported_dives(&table, &trips, false, false, true);
	QCOMPARE(parse_file(SUBSURFACE_TEST_DATA "/dives/test47.xml", &table, &trips), 0);
	add_imported_dives(&table, &trips, false, false, true);
	QCOMPARE(save_dives("./testmerge47+48.ssrf"), 0);
	QFile org(SUBSURFACE_TEST_DATA "/dives/test47+48.xml");
	org.open(QFile::ReadOnly);
	QFile out("./testmerge47+48.ssrf");
	out.open(QFile::ReadOnly);
	QTextStream orgS(&org);
	QTextStream outS(&out);
	QStringList readin = orgS.readAll().split("\n");
	QStringList written = outS.readAll().split("\n");
	while (readin.size() && written.size()) {
		QCOMPARE(written.takeFirst().trimmed(), readin.takeFirst().trimmed());
	}
}

QTEST_GUILESS_MAIN(TestMerge)
