#include "amqp_queue.h"
#include "amqp_queue_p.h"
#include "amqp_exchange.h"

using namespace QAMQP;

#include <QCoreApplication>
#include <QDebug>
#include <QDataStream>
#include <QFile>

Queue::Queue(int channelNumber, Client *parent)
    : Channel(new QueuePrivate(this), parent)
{
    Q_D(Queue);
    d->init(channelNumber, parent);
}

Queue::~Queue()
{
    remove();
}

void Queue::onOpen()
{
    Q_D(Queue);
    if (d->delayedDeclare)
        declare();

    if (!d->delayedBindings.isEmpty()) {
        typedef QPair<QString, QString> BindingPair;
        foreach(BindingPair binding, d->delayedBindings)
            bind(binding.first, binding.second);
        d->delayedBindings.clear();
    }
}

void Queue::onClose()
{
    remove(true, true);
}

Queue::QueueOptions Queue::option() const
{
    Q_D(const Queue);
    return d->options;
}

void Queue::setNoAck(bool noAck)
{
    Q_D(Queue);
    d->noAck = noAck;
}

bool Queue::noAck() const
{
    Q_D(const Queue);
    return d->noAck;
}

void Queue::declare(const QString &name, QueueOptions options)
{
    Q_D(Queue);
    if (!name.isEmpty())
        d->name = name;
    d->options = options;

    if (!d->opened) {
        d->delayedDeclare = true;
        return;
    }

    Frame::Method frame(Frame::fcQueue, QueuePrivate::miDeclare);
    frame.setChannel(d->number);

    QByteArray arguments;
    QDataStream out(&arguments, QIODevice::WriteOnly);

    out << qint16(0);   //reserver 1
    Frame::writeField('s', out, d->name);

    out << qint8(options);
    Frame::writeField('F', out, Frame::TableField());

    frame.setArguments(arguments);
    d->sendFrame(frame);
    d->delayedDeclare = false;
}

void Queue::remove(bool ifUnused, bool ifEmpty, bool noWait)
{
    Q_D(Queue);
    if (!d->declared) {
        qDebug() << Q_FUNC_INFO << "trying to remove undeclared queue, aborting...";
        return;
    }

    Frame::Method frame(Frame::fcQueue, QueuePrivate::miDelete);
    frame.setChannel(d->number);

    QByteArray arguments;
    QDataStream out(&arguments, QIODevice::WriteOnly);

    out << qint16(0);   //reserver 1
    Frame::writeField('s', out, d->name);

    qint8 flag = 0;
    flag |= (ifUnused ? 0x1 : 0);
    flag |= (ifEmpty ? 0x2 : 0);
    flag |= (noWait ? 0x4 : 0);
    out << flag;

    frame.setArguments(arguments);
    d->sendFrame(frame);
}

void Queue::purge()
{
    Q_D(Queue);

    if (!d->opened)
        return;

    Frame::Method frame(Frame::fcQueue, QueuePrivate::miPurge);
    frame.setChannel(d->number);

    QByteArray arguments;
    QDataStream out(&arguments, QIODevice::WriteOnly);

    out << qint16(0);   //reserver 1
    Frame::writeField('s', out, d->name);

    out << qint8(0);    // no-wait
    frame.setArguments(arguments);

    d->sendFrame(frame);
}

void Queue::bind(Exchange *exchange, const QString &key)
{
    if (!exchange) {
        qDebug() << Q_FUNC_INFO << "invalid exchange provided";
        return;
    }

    bind(exchange->name(), key);
}

void Queue::bind(const QString &exchangeName, const QString &key)
{
    Q_D(Queue);
    if (!d->opened) {
        d->delayedBindings.append(QPair<QString,QString>(exchangeName, key));
        return;
    }

    Frame::Method frame(Frame::fcQueue, QueuePrivate::miBind);
    frame.setChannel(d->number);

    QByteArray arguments;
    QDataStream out(&arguments, QIODevice::WriteOnly);

    out << qint16(0);   //reserver 1
    Frame::writeField('s', out, d->name);
    Frame::writeField('s', out, exchangeName);
    Frame::writeField('s', out, key);

    out << qint8(0);    // no-wait
    Frame::writeField('F', out, Frame::TableField());

    frame.setArguments(arguments);
    d->sendFrame(frame);
}

void Queue::unbind(Exchange *exchange, const QString &key)
{
    if (!exchange) {
        qDebug() << Q_FUNC_INFO << "invalid exchange provided";
        return;
    }

    unbind(exchange->name(), key);
}

void Queue::unbind(const QString &exchangeName, const QString &key)
{
    Q_D(Queue);
    if (!d->opened) {
        qDebug() << Q_FUNC_INFO << "queue is not open";
        return;
    }

    Frame::Method frame(Frame::fcQueue, QueuePrivate::miUnbind);
    frame.setChannel(d->number);

    QByteArray arguments;
    QDataStream out(&arguments, QIODevice::WriteOnly);
    out << qint16(0);   //reserver 1
    Frame::writeField('s', out, d->name);
    Frame::writeField('s', out, exchangeName);
    Frame::writeField('s', out, key);
    Frame::writeField('F', out, Frame::TableField());

    frame.setArguments(arguments);
    d->sendFrame(frame);
}

void Queue::_q_content(const Frame::Content &frame)
{
    Q_D(Queue);
    Q_ASSERT(frame.channel() == d->number);
    if (frame.channel() != d->number)
        return;

    if (d->messages.isEmpty()) {
        qErrnoWarning("Received content-header without method frame before");
        return;
    }

    MessagePtr &message = d->messages.last();
    message->leftSize = frame.bodySize();
    QHash<int, QVariant>::ConstIterator it;
    QHash<int, QVariant>::ConstIterator itEnd = frame.properties_.constEnd();
    for (it = frame.properties_.constBegin(); it != itEnd; ++it)
        message->property[Message::MessageProperty(it.key())] = it.value();
}

void Queue::_q_body(const Frame::ContentBody &frame)
{
    Q_D(Queue);
    Q_ASSERT(frame.channel() == d->number);
    if (frame.channel() != d->number)
        return;

    if (d->messages.isEmpty()) {
        qErrnoWarning("Received content-body without method frame before");
        return;
    }

    MessagePtr &message = d->messages.last();
    message->payload.append(frame.body());
    message->leftSize -= frame.body().size();

    if (message->leftSize == 0 && d->messages.size() == 1)
        Q_EMIT messageReceived(this);
}

MessagePtr Queue::getMessage()
{
    Q_D(Queue);
    return d->messages.dequeue();
}

bool Queue::hasMessage() const
{
    Q_D(const Queue);
    if (d->messages.isEmpty())
        return false;

    const MessagePtr &q = d->messages.head();
    return q->leftSize == 0;
}

void Queue::consume(ConsumeOptions options)
{
    Q_D(Queue);
    if (!d->opened) {
        qDebug() << Q_FUNC_INFO << "queue is not open";
        return;
    }

    Frame::Method frame(Frame::fcBasic, QueuePrivate::bmConsume);
    frame.setChannel(d->number);

    QByteArray arguments;
    QDataStream out(&arguments, QIODevice::WriteOnly);

    out << qint16(0);   //reserver 1
    Frame::writeField('s', out, d->name);
    Frame::writeField('s', out, d->consumerTag);

    out << qint8(options);  // no-wait
    Frame::writeField('F', out, Frame::TableField());

    frame.setArguments(arguments);
    d->sendFrame(frame);
}

void Queue::setConsumerTag(const QString &consumerTag)
{
    Q_D(Queue);
    d->consumerTag = consumerTag;
}

QString Queue::consumerTag() const
{
    Q_D(const Queue);
    return d->consumerTag;
}

void Queue::get()
{
    Q_D(Queue);
    if (!d->opened) {
        qDebug() << Q_FUNC_INFO << "queue is not open";
        return;
    }

    Frame::Method frame(Frame::fcBasic, QueuePrivate::bmGet);
    frame.setChannel(d->number);

    QByteArray arguments;
    QDataStream out(&arguments, QIODevice::WriteOnly);

    out << qint16(0);   //reserver 1
    Frame::writeField('s', out, d->name);

    out << qint8(d->noAck ? 1 : 0);    // noAck

    frame.setArguments(arguments);
    d->sendFrame(frame);
}

void Queue::ack(const MessagePtr &message)
{
    Q_D(Queue);
    if (!d->opened) {
        qDebug() << Q_FUNC_INFO << "queue is not open";
        return;
    }

    Frame::Method frame(Frame::fcBasic, QueuePrivate::bmAck);
    frame.setChannel(d->number);

    QByteArray arguments;
    QDataStream out(&arguments, QIODevice::WriteOnly);

    out << message->deliveryTag;    //reserver 1
    out << qint8(0);    // noAck

    frame.setArguments(arguments);
    d->sendFrame(frame);
}

//////////////////////////////////////////////////////////////////////////

QueuePrivate::QueuePrivate(Queue *q)
    : ChannelPrivate(q),
      delayedDeclare(false),
      declared(false),
      noAck(true),
      recievingMessage(false)
{
}

QueuePrivate::~QueuePrivate()
{
}

bool QueuePrivate::_q_method(const Frame::Method &frame)
{
    Q_Q(Queue);
    if (ChannelPrivate::_q_method(frame))
        return true;

    if (frame.methodClass() == Frame::fcQueue) {
        switch (frame.id()) {
        case miDeclareOk:
            declareOk(frame);
            break;
        case miDelete:
            deleteOk(frame);
            break;
        case miBindOk:
            bindOk(frame);
            break;
        case miUnbindOk:
            unbindOk(frame);
            break;
        case miPurgeOk:
            deleteOk(frame);
            break;
        default:
            break;
        }

        return true;
    }

    if (frame.methodClass() == Frame::fcBasic) {
        switch(frame.id()) {
        case bmConsumeOk:
            consumeOk(frame);
            break;
        case bmDeliver:
            deliver(frame);
            break;
        case bmGetOk:
            getOk(frame);
            break;
        case bmGetEmpty:
            QMetaObject::invokeMethod(q, "empty");
            break;
        default:
            break;
        }
        return true;
    }

    return false;
}

void QueuePrivate::declareOk(const Frame::Method &frame)
{
    Q_Q(Queue);
    qDebug() << "Declared queue: " << name;
    declared = true;

    QByteArray data = frame.arguments();
    QDataStream stream(&data, QIODevice::ReadOnly);

    name = Frame::readField('s', stream).toString();
    qint32 messageCount = 0, consumerCount = 0;
    stream >> messageCount >> consumerCount;
    qDebug("Message count %d\nConsumer count: %d", messageCount, consumerCount);

    QMetaObject::invokeMethod(q, "declared");
}

void QueuePrivate::deleteOk(const Frame::Method &frame)
{
    Q_Q(Queue);
    qDebug() << "Deleted or purged queue: " << name;
    declared = false;

    QByteArray data = frame.arguments();
    QDataStream stream(&data, QIODevice::ReadOnly);
    qint32 messageCount = 0;
    stream >> messageCount;
    qDebug("Message count %d", messageCount);
    QMetaObject::invokeMethod(q, "removed");
}

void QueuePrivate::bindOk(const Frame::Method &frame)
{
    Q_UNUSED(frame)
    Q_Q(Queue);

    qDebug() << "bound to queue: " << name;
    QMetaObject::invokeMethod(q, "binded", Q_ARG(bool, true));
}

void QueuePrivate::unbindOk(const Frame::Method &frame)
{
    Q_UNUSED(frame)
    Q_Q(Queue);

    qDebug() << "unbound queue: " << name;
    QMetaObject::invokeMethod(q, "binded", Q_ARG(bool, false));
}

void QueuePrivate::getOk(const Frame::Method &frame)
{
    QByteArray data = frame.arguments();
    QDataStream in(&data, QIODevice::ReadOnly);

    qlonglong deliveryTag = Frame::readField('L',in).toLongLong();
    bool redelivered = Frame::readField('t',in).toBool();
    QString exchangeName = Frame::readField('s',in).toString();
    QString routingKey = Frame::readField('s',in).toString();

    Q_UNUSED(redelivered)

    MessagePtr newMessage = MessagePtr(new Message);
    newMessage->routeKey = routingKey;
    newMessage->exchangeName = exchangeName;
    newMessage->deliveryTag = deliveryTag;
    messages.enqueue(newMessage);
}

void QueuePrivate::consumeOk(const Frame::Method &frame)
{
    qDebug() << "Consume ok: " << name;
    declared = false;

    QByteArray data = frame.arguments();
    QDataStream stream(&data, QIODevice::ReadOnly);
    consumerTag = Frame::readField('s',stream).toString();
    qDebug("Consumer tag = %s", qPrintable(consumerTag));
}

void QueuePrivate::deliver(const Frame::Method &frame)
{
    QByteArray data = frame.arguments();
    QDataStream in(&data, QIODevice::ReadOnly);
    QString consumer_ = Frame::readField('s',in).toString();
    if (consumer_ != consumerTag)
        return;

    qlonglong deliveryTag = Frame::readField('L',in).toLongLong();
    bool redelivered = Frame::readField('t',in).toBool();
    QString exchangeName = Frame::readField('s',in).toString();
    QString routingKey = Frame::readField('s',in).toString();

    Q_UNUSED(redelivered)

    MessagePtr newMessage = MessagePtr(new Message);
    newMessage->routeKey = routingKey;
    newMessage->exchangeName = exchangeName;
    newMessage->deliveryTag = deliveryTag;
    messages.enqueue(newMessage);
}