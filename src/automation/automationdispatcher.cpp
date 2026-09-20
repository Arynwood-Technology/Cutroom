/*
    SPDX-FileCopyrightText: 2026 Arynwood Technology
    SPDX-License-Identifier: GPL-3.0-only
*/

#include "automationdispatcher.h"

#include <QJsonValue>
#include <QList>
#include <QMetaMethod>
#include <QMetaObject>
#include <QMetaType>
#include <QObject>
#include <QStringList>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace Automation {

namespace {

QJsonObject errorObject(int code, const QString &message)
{
    QJsonObject error;
    error.insert(QStringLiteral("code"), code);
    error.insert(QStringLiteral("message"), message);
    QJsonObject reply;
    reply.insert(QStringLiteral("error"), error);
    return reply;
}

bool isWholeNumber(const QJsonValue &value, int *out)
{
    if (!value.isDouble()) {
        return false;
    }
    const double number = value.toDouble();
    if (!std::isfinite(number) || number != std::floor(number) || number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max()) {
        return false;
    }
    *out = static_cast<int>(number);
    return true;
}

bool isIntList(QMetaType type)
{
    return type == QMetaType::fromType<QList<int>>();
}

bool isSupportedParameter(QMetaType type)
{
    switch (type.id()) {
    case QMetaType::Bool:
    case QMetaType::Int:
    case QMetaType::Double:
    case QMetaType::QString:
    case QMetaType::QStringList:
        return true;
    default:
        return isIntList(type);
    }
}

bool isSupportedResult(QMetaType type)
{
    switch (type.id()) {
    case QMetaType::Void:
    case QMetaType::QVariantList:
    case QMetaType::QVariantMap:
        return true;
    default:
        return isSupportedParameter(type);
    }
}

/** Convert one JSON argument to a QVariant holding exactly @p type. Fills @p error and returns false when it does not fit. */
bool toVariant(const QJsonValue &value, QMetaType type, QVariant *out, QString *error)
{
    switch (type.id()) {
    case QMetaType::Bool:
        if (!value.isBool()) {
            *error = QStringLiteral("expected true or false");
            return false;
        }
        *out = QVariant::fromValue(value.toBool());
        return true;
    case QMetaType::Int: {
        int number = 0;
        if (!isWholeNumber(value, &number)) {
            *error = QStringLiteral("expected a whole number that fits in 32 bits");
            return false;
        }
        *out = QVariant::fromValue(number);
        return true;
    }
    case QMetaType::Double:
        if (!value.isDouble()) {
            *error = QStringLiteral("expected a number");
            return false;
        }
        *out = QVariant::fromValue(value.toDouble());
        return true;
    case QMetaType::QString:
        if (!value.isString()) {
            *error = QStringLiteral("expected a string");
            return false;
        }
        *out = QVariant::fromValue(value.toString());
        return true;
    case QMetaType::QStringList: {
        if (!value.isArray()) {
            *error = QStringLiteral("expected a list of strings");
            return false;
        }
        QStringList list;
        const QJsonArray array = value.toArray();
        for (const QJsonValue &item : array) {
            if (!item.isString()) {
                *error = QStringLiteral("expected a list of strings");
                return false;
            }
            list << item.toString();
        }
        *out = QVariant::fromValue(list);
        return true;
    }
    default:
        break;
    }
    if (isIntList(type)) {
        if (!value.isArray()) {
            *error = QStringLiteral("expected a list of whole numbers");
            return false;
        }
        QList<int> list;
        const QJsonArray array = value.toArray();
        for (const QJsonValue &item : array) {
            int number = 0;
            if (!isWholeNumber(item, &number)) {
                *error = QStringLiteral("expected a list of whole numbers that fit in 32 bits");
                return false;
            }
            list << number;
        }
        *out = QVariant::fromValue(list);
        return true;
    }
    *error = QStringLiteral("unsupported parameter type");
    return false;
}

QJsonValue toJson(const QVariant &value, QMetaType type)
{
    switch (type.id()) {
    case QMetaType::Void:
        return QJsonValue(QJsonValue::Null);
    case QMetaType::Bool:
        return QJsonValue(value.toBool());
    case QMetaType::Int:
        return QJsonValue(value.toInt());
    case QMetaType::Double:
        return QJsonValue(value.toDouble());
    case QMetaType::QString:
        return QJsonValue(value.toString());
    case QMetaType::QStringList:
    case QMetaType::QVariantList:
    case QMetaType::QVariantMap:
        return QJsonValue::fromVariant(value);
    default:
        break;
    }
    QJsonArray array;
    const QList<int> list = value.value<QList<int>>();
    for (int item : list) {
        array.append(item);
    }
    return array;
}

QString typeName(QMetaType type)
{
    return type.isValid() ? QString::fromLatin1(type.name()) : QStringLiteral("unknown");
}

bool isOffered(const QMetaMethod &method, const QString &prefix)
{
    if (method.access() != QMetaMethod::Public) {
        return false;
    }
    if (method.methodType() != QMetaMethod::Slot && method.methodType() != QMetaMethod::Method) {
        return false;
    }
    return QString::fromLatin1(method.name()).startsWith(prefix);
}

} // namespace

bool Dispatcher::supportsSignature(const QMetaMethod &method)
{
    if (!isSupportedResult(method.returnMetaType())) {
        return false;
    }
    for (int i = 0; i < method.parameterCount(); ++i) {
        if (!isSupportedParameter(method.parameterMetaType(i))) {
            return false;
        }
    }
    return true;
}

Dispatcher::Dispatcher(QObject *target, const QString &prefix)
    : m_target(target)
    , m_prefix(prefix)
{
}

QJsonArray Dispatcher::listMethods() const
{
    struct Entry {
        QString name;
        QJsonObject json;
    };
    std::vector<Entry> entries;
    const QMetaObject *meta = m_target->metaObject();
    for (int i = 0; i < meta->methodCount(); ++i) {
        const QMetaMethod method = meta->method(i);
        if (!isOffered(method, m_prefix)) {
            continue;
        }
        QJsonArray params;
        const QList<QByteArray> names = method.parameterNames();
        for (int p = 0; p < method.parameterCount(); ++p) {
            QJsonObject param;
            param.insert(QStringLiteral("name"), p < names.size() ? QString::fromLatin1(names.at(p)) : QString());
            param.insert(QStringLiteral("type"), typeName(method.parameterMetaType(p)));
            params.append(param);
        }
        QJsonObject json;
        json.insert(QStringLiteral("name"), QString::fromLatin1(method.name()));
        json.insert(QStringLiteral("params"), params);
        json.insert(QStringLiteral("returns"), typeName(method.returnMetaType()));
        entries.push_back({QString::fromLatin1(method.name()), json});
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.name < b.name; });
    QJsonArray result;
    for (const Entry &entry : entries) {
        result.append(entry.json);
    }
    return result;
}

QJsonObject Dispatcher::call(const QString &method, const QJsonArray &args) const
{
    // Only prefixed methods are ever reachable; everything else on the object is "not found", whether or not it exists.
    if (!method.startsWith(m_prefix)) {
        return errorObject(MethodNotFound, QStringLiteral("no such method: %1").arg(method));
    }
    const QMetaObject *meta = m_target->metaObject();
    QMetaMethod chosen;
    QList<int> otherCounts;
    for (int i = 0; i < meta->methodCount(); ++i) {
        const QMetaMethod candidate = meta->method(i);
        if (!isOffered(candidate, m_prefix) || QString::fromLatin1(candidate.name()) != method) {
            continue;
        }
        if (candidate.parameterCount() == args.size()) {
            chosen = candidate;
            break;
        }
        otherCounts << candidate.parameterCount();
    }
    if (!chosen.isValid()) {
        if (otherCounts.isEmpty()) {
            return errorObject(MethodNotFound, QStringLiteral("no such method: %1").arg(method));
        }
        std::sort(otherCounts.begin(), otherCounts.end());
        QStringList counts;
        for (int count : std::as_const(otherCounts)) {
            counts << QString::number(count);
        }
        return errorObject(InvalidParams,
                           QStringLiteral("%1 takes %2 argument(s), got %3").arg(method, counts.join(QStringLiteral(" or ")), QString::number(args.size())));
    }
    // Refuse a signature we cannot represent before running anything, so a refused call has no side effects.
    if (!supportsSignature(chosen)) {
        return errorObject(InternalError, QStringLiteral("%1 uses a type the automation interface does not support").arg(method));
    }

    std::vector<QVariant> values;
    values.reserve(static_cast<size_t>(args.size()));
    for (int i = 0; i < args.size(); ++i) {
        QVariant value;
        QString problem;
        if (!toVariant(args.at(i), chosen.parameterMetaType(i), &value, &problem)) {
            const QList<QByteArray> names = chosen.parameterNames();
            const QString name = i < names.size() && !names.at(i).isEmpty() ? QString::fromLatin1(names.at(i)) : QString::number(i + 1);
            return errorObject(InvalidParams, QStringLiteral("argument %1 of %2: %3").arg(name, method, problem));
        }
        values.push_back(value);
    }

    const QMetaType returnType = chosen.returnMetaType();
    QVariant result;
    std::vector<void *> argv(static_cast<size_t>(args.size()) + 1, nullptr);
    if (returnType.id() != QMetaType::Void) {
        result = QVariant(returnType);
        argv[0] = result.data();
    }
    for (size_t i = 0; i < values.size(); ++i) {
        argv[i + 1] = values[i].data();
    }
    // Kdenlive is built without exceptions, so a scripting method cannot throw; there is nothing to catch here.
    QMetaObject::metacall(m_target, QMetaObject::InvokeMetaMethod, chosen.methodIndex(), argv.data());
    QJsonObject reply;
    reply.insert(QStringLiteral("result"), toJson(result, returnType));
    return reply;
}

} // namespace Automation
