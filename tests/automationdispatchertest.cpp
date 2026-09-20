/*
    SPDX-FileCopyrightText: 2026 Arynwood Technology
    SPDX-License-Identifier: GPL-3.0-only
*/

#include "catch.hpp"

#include "automation/automationdispatcher.h"
#include "mainwindow.h"

#include <QMetaMethod>
#include <QObject>
#include <QPoint>
#include <QRect>

using namespace Automation;

// A named namespace, not an anonymous one: moc does not reliably support Q_OBJECT classes in anonymous namespaces.
namespace AutomationMock {

/** Stands in for MainWindow: the same shapes of signature the scripting API uses, plus methods that must stay out of reach. */
class MockTarget : public QObject
{
    Q_OBJECT
public:
    int calls = 0;

public Q_SLOTS:
    int scriptAdd(int a, int b)
    {
        ++calls;
        return a + b;
    }
    bool scriptIsPositive(double value)
    {
        ++calls;
        return value > 0;
    }
    double scriptHalf(double value)
    {
        ++calls;
        return value / 2;
    }
    // Default arguments make moc generate one overload per argument count, as in MainWindow.
    QString scriptEcho(const QString &text, bool upper = false)
    {
        ++calls;
        return upper ? text.toUpper() : text;
    }
    QStringList scriptSplit(const QString &text)
    {
        ++calls;
        return text.split(QLatin1Char(','));
    }
    int scriptTotal(const QList<int> &ids)
    {
        ++calls;
        int total = 0;
        for (int id : ids) {
            total += id;
        }
        return total;
    }
    QStringList scriptJoinAll(const QStringList &parts)
    {
        ++calls;
        return QStringList(parts.join(QLatin1Char('+')));
    }
    QVariantList scriptList()
    {
        ++calls;
        return {1, QStringLiteral("two"), true};
    }
    QVariantMap scriptInfo()
    {
        ++calls;
        QVariantMap map;
        map.insert(QStringLiteral("fps"), 25.0);
        map.insert(QStringLiteral("name"), QStringLiteral("demo"));
        return map;
    }
    void scriptTouch()
    {
        ++calls;
    }
    // Seven parameters is the most any real scripting method takes.
    int scriptSeven(int a, int b, int c, int d, int e, int f, int g)
    {
        ++calls;
        return a + b + c + d + e + f + g;
    }
    // Not representable: refused before it runs.
    bool scriptTakesPoint(const QPoint &)
    {
        ++calls;
        return true;
    }
    QRect scriptReturnsRect()
    {
        ++calls;
        return QRect();
    }
    // Not offered: no "script" prefix. These are what a client must never reach.
    void notScripted()
    {
        ++calls;
    }

private Q_SLOTS:
    void scriptPrivate()
    {
        ++calls;
    }
Q_SIGNALS:
    void scriptSignal();
};

} // namespace AutomationMock

using AutomationMock::MockTarget;

namespace {

QJsonArray args(std::initializer_list<QJsonValue> values)
{
    QJsonArray array;
    for (const QJsonValue &value : values) {
        array.append(value);
    }
    return array;
}

int errorCode(const QJsonObject &reply)
{
    return reply.value(QLatin1String("error")).toObject().value(QLatin1String("code")).toInt();
}

QString errorMessage(const QJsonObject &reply)
{
    return reply.value(QLatin1String("error")).toObject().value(QLatin1String("message")).toString();
}

} // namespace

TEST_CASE("Scripting methods are listed with their types, and nothing else is", "[Automation]")
{
    MockTarget target;
    Dispatcher dispatcher(&target);
    const QJsonArray methods = dispatcher.listMethods();

    QStringList names;
    for (const QJsonValue &method : methods) {
        names << method.toObject().value(QLatin1String("name")).toString();
    }
    CHECK(names.contains(QLatin1String("scriptAdd")));
    CHECK(names.contains(QLatin1String("scriptTotal")));
    // Not offered: other slots, inherited QObject methods, private slots, signals.
    CHECK_FALSE(names.contains(QLatin1String("notScripted")));
    CHECK_FALSE(names.contains(QLatin1String("deleteLater")));
    CHECK_FALSE(names.contains(QLatin1String("scriptPrivate")));
    CHECK_FALSE(names.contains(QLatin1String("scriptSignal")));
    // Sorted by name.
    QStringList sorted = names;
    sorted.sort();
    CHECK(names == sorted);
    // One entry per overload: scriptEcho(text) and scriptEcho(text, upper).
    CHECK(names.count(QLatin1String("scriptEcho")) == 2);

    for (const QJsonValue &value : methods) {
        const QJsonObject method = value.toObject();
        if (method.value(QLatin1String("name")).toString() == QLatin1String("scriptAdd")) {
            CHECK(method.value(QLatin1String("returns")).toString() == QLatin1String("int"));
            const QJsonArray params = method.value(QLatin1String("params")).toArray();
            REQUIRE(params.size() == 2);
            CHECK(params.at(0).toObject().value(QLatin1String("name")).toString() == QLatin1String("a"));
            CHECK(params.at(0).toObject().value(QLatin1String("type")).toString() == QLatin1String("int"));
        }
        if (method.value(QLatin1String("name")).toString() == QLatin1String("scriptTouch")) {
            CHECK(method.value(QLatin1String("returns")).toString() == QLatin1String("void"));
        }
    }
}

TEST_CASE("Every supported type goes in and comes out as JSON", "[Automation]")
{
    MockTarget target;
    Dispatcher dispatcher(&target);

    CHECK(dispatcher.call(QStringLiteral("scriptAdd"), args({2, 3})).value(QLatin1String("result")).toInt() == 5);
    CHECK(dispatcher.call(QStringLiteral("scriptAdd"), args({-4, 1})).value(QLatin1String("result")).toInt() == -3);
    CHECK(dispatcher.call(QStringLiteral("scriptIsPositive"), args({0.5})).value(QLatin1String("result")).toBool());
    CHECK_FALSE(dispatcher.call(QStringLiteral("scriptIsPositive"), args({-1})).value(QLatin1String("result")).toBool());
    CHECK(dispatcher.call(QStringLiteral("scriptHalf"), args({5})).value(QLatin1String("result")).toDouble() == Approx(2.5));
    CHECK(dispatcher.call(QStringLiteral("scriptEcho"), args({QStringLiteral("héllo ✓")})).value(QLatin1String("result")).toString() == QStringLiteral("héllo ✓"));

    const QJsonArray split = dispatcher.call(QStringLiteral("scriptSplit"), args({QStringLiteral("a,b,c")})).value(QLatin1String("result")).toArray();
    CHECK(split.size() == 3);
    CHECK(split.at(2).toString() == QLatin1String("c"));

    QJsonArray ids;
    ids << 1 << 2 << 3 << 4;
    CHECK(dispatcher.call(QStringLiteral("scriptTotal"), args({ids})).value(QLatin1String("result")).toInt() == 10);
    CHECK(dispatcher.call(QStringLiteral("scriptTotal"), args({QJsonArray()})).value(QLatin1String("result")).toInt() == 0);

    QJsonArray parts;
    parts << QStringLiteral("x") << QStringLiteral("y");
    CHECK(dispatcher.call(QStringLiteral("scriptJoinAll"), args({parts})).value(QLatin1String("result")).toArray().at(0).toString() == QLatin1String("x+y"));

    const QJsonArray list = dispatcher.call(QStringLiteral("scriptList"), QJsonArray()).value(QLatin1String("result")).toArray();
    REQUIRE(list.size() == 3);
    CHECK(list.at(0).toInt() == 1);
    CHECK(list.at(1).toString() == QLatin1String("two"));
    CHECK(list.at(2).toBool());

    const QJsonObject info = dispatcher.call(QStringLiteral("scriptInfo"), QJsonArray()).value(QLatin1String("result")).toObject();
    CHECK(info.value(QLatin1String("fps")).toDouble() == Approx(25.0));
    CHECK(info.value(QLatin1String("name")).toString() == QLatin1String("demo"));

    // A void method succeeds with a null result, and runs exactly once.
    target.calls = 0;
    const QJsonObject touched = dispatcher.call(QStringLiteral("scriptTouch"), QJsonArray());
    CHECK(touched.contains(QLatin1String("result")));
    CHECK(touched.value(QLatin1String("result")).isNull());
    CHECK(target.calls == 1);

    CHECK(dispatcher.call(QStringLiteral("scriptSeven"), args({1, 2, 3, 4, 5, 6, 7})).value(QLatin1String("result")).toInt() == 28);
}

TEST_CASE("Overloads are chosen by argument count", "[Automation]")
{
    MockTarget target;
    Dispatcher dispatcher(&target);
    CHECK(dispatcher.call(QStringLiteral("scriptEcho"), args({QStringLiteral("abc")})).value(QLatin1String("result")).toString() == QLatin1String("abc"));
    CHECK(dispatcher.call(QStringLiteral("scriptEcho"), args({QStringLiteral("abc"), true})).value(QLatin1String("result")).toString() == QLatin1String("ABC"));

    const QJsonObject tooMany = dispatcher.call(QStringLiteral("scriptEcho"), args({QStringLiteral("a"), true, 3}));
    CHECK(errorCode(tooMany) == InvalidParams);
    // The message names the method, the counts it accepts and the count it got, with no placeholder left over.
    CHECK(errorMessage(tooMany).contains(QLatin1String("scriptEcho")));
    CHECK(errorMessage(tooMany).contains(QLatin1String("1 or 2")));
    CHECK(errorMessage(tooMany).contains(QLatin1String("got 3")));
    CHECK_FALSE(errorMessage(tooMany).contains(QLatin1Char('%')));
}

TEST_CASE("Only prefixed public methods can be reached", "[Automation]")
{
    MockTarget target;
    Dispatcher dispatcher(&target);
    target.calls = 0;

    // Real methods that must never be callable from outside.
    for (const QString &name : {QStringLiteral("notScripted"), QStringLiteral("deleteLater"), QStringLiteral("scriptPrivate"), QStringLiteral("scriptSignal"),
                                QStringLiteral("setObjectName"), QStringLiteral("nope"), QStringLiteral(""), QStringLiteral("script")}) {
        const QJsonObject reply = dispatcher.call(name, QJsonArray());
        INFO(name.toStdString());
        CHECK(errorCode(reply) == MethodNotFound);
        CHECK_FALSE(reply.contains(QLatin1String("result")));
    }
    CHECK(target.calls == 0);

    // The prefix is configurable, and applies to both listing and calling.
    Dispatcher other(&target, QStringLiteral("notS"));
    CHECK(other.call(QStringLiteral("notScripted"), QJsonArray()).contains(QLatin1String("result")));
    CHECK(errorCode(other.call(QStringLiteral("scriptAdd"), args({1, 2}))) == MethodNotFound);
    CHECK(other.listMethods().size() == 1);
}

TEST_CASE("Wrong types are refused, never coerced, and nothing runs", "[Automation]")
{
    MockTarget target;
    Dispatcher dispatcher(&target);
    target.calls = 0;

    CHECK(errorCode(dispatcher.call(QStringLiteral("scriptAdd"), args({QStringLiteral("2"), 3}))) == InvalidParams);
    CHECK(errorCode(dispatcher.call(QStringLiteral("scriptAdd"), args({1.5, 3}))) == InvalidParams);
    CHECK(errorCode(dispatcher.call(QStringLiteral("scriptAdd"), args({3e10, 1}))) == InvalidParams);
    CHECK(errorCode(dispatcher.call(QStringLiteral("scriptAdd"), args({true, 1}))) == InvalidParams);
    CHECK(errorCode(dispatcher.call(QStringLiteral("scriptAdd"), args({QJsonValue(QJsonValue::Null), 1}))) == InvalidParams);
    CHECK(errorCode(dispatcher.call(QStringLiteral("scriptIsPositive"), args({QStringLiteral("1")}))) == InvalidParams);
    CHECK(errorCode(dispatcher.call(QStringLiteral("scriptEcho"), args({7}))) == InvalidParams);
    CHECK(errorCode(dispatcher.call(QStringLiteral("scriptEcho"), args({QStringLiteral("a"), 1}))) == InvalidParams);

    QJsonArray mixed;
    mixed << 1 << QStringLiteral("2");
    CHECK(errorCode(dispatcher.call(QStringLiteral("scriptTotal"), args({mixed}))) == InvalidParams);
    QJsonArray fractional;
    fractional << 1 << 2.5;
    CHECK(errorCode(dispatcher.call(QStringLiteral("scriptTotal"), args({fractional}))) == InvalidParams);
    CHECK(errorCode(dispatcher.call(QStringLiteral("scriptTotal"), args({5}))) == InvalidParams);
    QJsonArray notStrings;
    notStrings << QStringLiteral("a") << 3;
    CHECK(errorCode(dispatcher.call(QStringLiteral("scriptJoinAll"), args({notStrings}))) == InvalidParams);

    // The message says which argument was wrong.
    const QString message = errorMessage(dispatcher.call(QStringLiteral("scriptAdd"), args({1, QStringLiteral("x")})));
    CHECK(message.contains(QLatin1String("argument b of scriptAdd")));

    CHECK(target.calls == 0);
}

TEST_CASE("A signature that cannot be represented is refused before it runs", "[Automation]")
{
    MockTarget target;
    Dispatcher dispatcher(&target);
    target.calls = 0;

    CHECK(errorCode(dispatcher.call(QStringLiteral("scriptTakesPoint"), args({1}))) == InternalError);
    CHECK(errorCode(dispatcher.call(QStringLiteral("scriptReturnsRect"), QJsonArray())) == InternalError);
    // Neither method's body ran: a refused call has no side effects.
    CHECK(target.calls == 0);
}

TEST_CASE("The scripting interface keeps its D-Bus name", "[Automation]")
{
    // QtDBus derives the name from the organisation domain unless it is pinned, and main.cpp changes that domain. If this fails, every
    // scripting client (kdenlive-api, mcp-kdenlive) stops working: the interface name is part of the public API.
    const QMetaObject &meta = MainWindow::staticMetaObject;
    const int index = meta.indexOfClassInfo("D-Bus Interface");
    REQUIRE(index >= meta.classInfoOffset());
    CHECK(QString::fromLatin1(meta.classInfo(index).value()) == QLatin1String("org.kde.kdenlive.MainWindow"));
}

TEST_CASE("Every scripting method of the real MainWindow can be carried as JSON", "[Automation]")
{
    // The design rests on this: the whole surface uses only types the dispatcher handles, so one generic call() can serve all of it.
    const QMetaObject &meta = MainWindow::staticMetaObject;
    int scripting = 0;
    QStringList refused;
    for (int i = meta.methodOffset(); i < meta.methodCount(); ++i) {
        const QMetaMethod method = meta.method(i);
        if (method.access() != QMetaMethod::Public || (method.methodType() != QMetaMethod::Slot && method.methodType() != QMetaMethod::Method)
            || !QString::fromLatin1(method.name()).startsWith(QLatin1String("script"))) {
            continue;
        }
        ++scripting;
        if (!Dispatcher::supportsSignature(method)) {
            refused << QString::fromLatin1(method.methodSignature());
        }
    }
    // A sanity floor, so the loop cannot pass by matching nothing.
    CHECK(scripting > 150);
    INFO("methods the dispatcher would refuse: " << refused.join(QLatin1String(", ")).toStdString());
    CHECK(refused.isEmpty());
}

#include "automationdispatchertest.moc"
