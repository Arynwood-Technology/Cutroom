/*
    SPDX-FileCopyrightText: 2026 Arynwood Technology
    SPDX-License-Identifier: GPL-3.0-only
*/

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

class QMetaMethod;
class QObject;

/** @namespace Automation
 *  @brief The parts of the Cutroom Automation Server that need no network: see dev-docs/release-architecture.md, decision D3. */
namespace Automation {

/** JSON-RPC 2.0 error codes used in the "error" object returned by Dispatcher::call(). */
enum ErrorCode {
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603,
};

/** @class Dispatcher
 *  @brief Calls the scripting methods of one QObject from JSON, and describes them as JSON.
 *
 *  A method is offered when it is a public slot or Q_INVOKABLE and its name starts with the prefix ("script" by default, the naming
 *  convention of the MainWindow scripting API). Nothing else on the object can be reached, whatever name a client sends: a request for
 *  deleteLater() or close() is "method not found".
 *
 *  Parameters and results are limited to the types the scripting API uses: bool, int, double, QString, QStringList and QList<int> in;
 *  those plus QVariantList and QVariantMap, or void, out. A value of the wrong JSON type, a fractional or out-of-range int, or a method
 *  whose signature uses any other type is refused with an error and never coerced. The signature is checked before the method runs,
 *  so a refused call has no side effects.
 *
 *  Not thread-safe: call it on the target's thread. */
class Dispatcher
{
public:
    explicit Dispatcher(QObject *target, const QString &prefix = QStringLiteral("script"));

    /** @brief Whether every parameter and the result of @p method are types this dispatcher can carry. call() refuses any other method. */
    static bool supportsSignature(const QMetaMethod &method);

    /** @brief The offered methods, sorted by name, one entry per overload:
     *  {"name": ..., "params": [{"name": ..., "type": ...}], "returns": ...}. */
    QJsonArray listMethods() const;

    /** @brief Call @p method with @p args (matched to an overload by count).
     *  @return {"result": value} on success, {"error": {"code": ..., "message": ...}} otherwise. */
    QJsonObject call(const QString &method, const QJsonArray &args) const;

private:
    QObject *m_target;
    QString m_prefix;
};

} // namespace Automation
