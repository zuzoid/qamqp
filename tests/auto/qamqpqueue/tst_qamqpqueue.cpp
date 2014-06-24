#include <QScopedPointer>

#include <QtTest/QtTest>
#include "amqp_testcase.h"

#include "amqp_client.h"
#include "amqp_queue.h"
#include "amqp_exchange.h"

using namespace QAMQP;
class tst_QAMQPQueue : public TestCase
{
    Q_OBJECT
private Q_SLOTS:
    void init();
    void cleanup();

    void defaultExchange();
    void standardExchanges_data();
    void standardExchanges();
    void invalidDeclaration_data();
    void invalidDeclaration();
    void invalidBind();
    void unnamed();
    void exclusiveAccess();
    void exclusiveRemoval();
    void notFound();
    void remove();
    void removeIfUnused();
    void removeIfEmpty();
    void unbind();
    void purge();
    void canOnlyStartConsumingOnce();
    void cancel();
    void invalidCancelBecauseNotConsuming();
    void invalidCancelBecauseInvalidConsumerTag();

private:
    QScopedPointer<Client> client;

};

void tst_QAMQPQueue::init()
{
    client.reset(new Client);
    client->connectToHost();
    QVERIFY(waitForSignal(client.data(), SIGNAL(connected())));
}

void tst_QAMQPQueue::cleanup()
{
    if (client->isConnected()) {
        client->disconnectFromHost();
        QVERIFY(waitForSignal(client.data(), SIGNAL(disconnected())));
    }
}

void tst_QAMQPQueue::defaultExchange()
{
    Queue *queue = client->createQueue("test-default-exchange");
    queue->declare();
    QVERIFY(waitForSignal(queue, SIGNAL(declared())));
    QVERIFY(queue->consume());
    QVERIFY(waitForSignal(queue, SIGNAL(consuming(QString))));

    Exchange *defaultExchange = client->createExchange();
    defaultExchange->publish("first message", "test-default-exchange");
    QVERIFY(waitForSignal(queue, SIGNAL(messageReceived())));
    Message message = queue->dequeue();
    QCOMPARE(message.payload(), QByteArray("first message"));
}

void tst_QAMQPQueue::standardExchanges_data()
{
    QTest::addColumn<QString>("exchange");
    QTest::newRow("amq.direct") << "amq.direct";
    QTest::newRow("amq.fanout") << "amq.fanout";
    QTest::newRow("amq.headers") << "amq.headers";
    QTest::newRow("amq.match") << "amq.match";
    QTest::newRow("amq.topic") << "amq.topic";
}

void tst_QAMQPQueue::standardExchanges()
{
    QFETCH(QString, exchange);

    QString queueName = QString("test-%1").arg(exchange);
    QString routingKey = QString("testRoutingKey-%1").arg(exchange);

    Queue *queue = client->createQueue(queueName);
    queue->declare();
    QVERIFY(waitForSignal(queue, SIGNAL(declared())));
    QVERIFY(queue->consume());   // required because AutoDelete will not delete if
                                 // there was never a consumer
    QVERIFY(waitForSignal(queue, SIGNAL(consuming(QString))));

    queue->bind(exchange, routingKey);
    QVERIFY(waitForSignal(queue, SIGNAL(bound())));

    Exchange *defaultExchange = client->createExchange(exchange);
    defaultExchange->publish("test message", routingKey);
    QVERIFY(waitForSignal(queue, SIGNAL(messageReceived())));
    QCOMPARE(queue->dequeue().payload(), QByteArray("test message"));
}

void tst_QAMQPQueue::invalidDeclaration_data()
{
    QTest::addColumn<QString>("queueName");
    QTest::addColumn<QAMQP::Error>("error");

    QTest::newRow("amq.direct") << "amq.direct" <<  QAMQP::AccessRefusedError;
    QTest::newRow("amq.fanout") << "amq.fanout" << QAMQP::AccessRefusedError;
    QTest::newRow("amq.headers") << "amq.headers" << QAMQP::AccessRefusedError;
    QTest::newRow("amq.match") << "amq.match" << QAMQP::AccessRefusedError;
    QTest::newRow("amq.topic") << "amq.topic" << QAMQP::AccessRefusedError;
    QTest::newRow("amq.reserved") << "amq.reserved" << QAMQP::AccessRefusedError;
}

void tst_QAMQPQueue::invalidDeclaration()
{
    QFETCH(QString, queueName);
    QFETCH(QAMQP::Error, error);

    Queue *queue = client->createQueue(queueName);
    queue->declare();
    QVERIFY(waitForSignal(queue, SIGNAL(error(QAMQP::Error))));
    QCOMPARE(queue->error(), error);
}

void tst_QAMQPQueue::invalidBind()
{
    Queue *queue = client->createQueue("test-invalid-bind");
    queue->declare();
    QVERIFY(waitForSignal(queue, SIGNAL(declared())));
    QVERIFY(queue->consume());   // for autodelete
    QVERIFY(waitForSignal(queue, SIGNAL(consuming(QString))));

    queue->bind("non-existant-exchange", "routingKey");
    QVERIFY(waitForSignal(queue, SIGNAL(error(QAMQP::Error))));
    QCOMPARE(queue->error(), QAMQP::NotFoundError);
}

void tst_QAMQPQueue::unnamed()
{
    Queue *queue = client->createQueue();
    queue->declare();
    QVERIFY(waitForSignal(queue, SIGNAL(declared())));
    QVERIFY(queue->consume());
    QVERIFY(waitForSignal(queue, SIGNAL(consuming(QString))));

    QVERIFY(!queue->name().isEmpty());
}

void tst_QAMQPQueue::exclusiveAccess()
{
    Queue *queue = client->createQueue("test-exclusive-queue");
    queue->declare(Queue::Exclusive);
    QVERIFY(waitForSignal(queue, SIGNAL(declared())));

    Client secondClient;
    secondClient.connectToHost();
    QVERIFY(waitForSignal(&secondClient, SIGNAL(connected())));
    Queue *passiveQueue = secondClient.createQueue("test-exclusive-queue");
    passiveQueue->declare(Queue::Passive);
    QVERIFY(waitForSignal(passiveQueue, SIGNAL(error(QAMQP::Error))));
    QCOMPARE(passiveQueue->error(), QAMQP::ResourceLockedError);

    secondClient.disconnectFromHost();
    QVERIFY(waitForSignal(&secondClient, SIGNAL(disconnected())));
}

void tst_QAMQPQueue::exclusiveRemoval()
{
    Queue *queue = client->createQueue("test-exclusive-queue");
    queue->declare(Queue::Exclusive);
    QVERIFY(waitForSignal(queue, SIGNAL(declared())));
    client.data()->disconnectFromHost();
    QVERIFY(waitForSignal(client.data(), SIGNAL(disconnected())));

    // create a new client and try to access the queue that should
    // no longer exist
    Client secondClient;
    secondClient.connectToHost();
    QVERIFY(waitForSignal(&secondClient, SIGNAL(connected())));
    Queue *passiveQueue = secondClient.createQueue("test-exclusive-queue");
    passiveQueue->declare(Queue::Passive);
    QVERIFY(waitForSignal(passiveQueue, SIGNAL(error(QAMQP::Error))));
    QCOMPARE(passiveQueue->error(), QAMQP::NotFoundError);
    secondClient.disconnectFromHost();
    QVERIFY(waitForSignal(&secondClient, SIGNAL(disconnected())));
}

void tst_QAMQPQueue::notFound()
{
    Queue *queue = client->createQueue("test-not-found");
    queue->declare(Queue::Passive);
    QVERIFY(waitForSignal(queue, SIGNAL(error(QAMQP::Error))));
    QCOMPARE(queue->error(), QAMQP::NotFoundError);
}

void tst_QAMQPQueue::remove()
{
    Queue *queue = client->createQueue("test-remove");
    queue->declare();
    QVERIFY(waitForSignal(queue, SIGNAL(declared())));
    queue->remove(Queue::roIfEmpty|Queue::roIfUnused);
    QVERIFY(waitForSignal(queue, SIGNAL(removed())));
}

void tst_QAMQPQueue::removeIfUnused()
{
    Queue *queue = client->createQueue("test-remove-if-unused");
    queue->declare();
    QVERIFY(waitForSignal(queue, SIGNAL(declared())));
    QVERIFY(queue->consume());
    QVERIFY(waitForSignal(queue, SIGNAL(consuming(QString))));

    queue->remove(Queue::roIfUnused);
    QVERIFY(waitForSignal(queue, SIGNAL(error(QAMQP::Error))));
    QCOMPARE(queue->error(), QAMQP::PreconditionFailedError);
    QVERIFY(!queue->errorString().isEmpty());
}

void tst_QAMQPQueue::removeIfEmpty()
{
    // declare the queue and send messages to it
    Queue *queue = client->createQueue("test-remove-if-empty");
    queue->declare(Queue::Durable);
    QVERIFY(waitForSignal(queue, SIGNAL(declared())));
    Exchange *defaultExchange = client->createExchange();
    defaultExchange->publish("first message", "test-remove-if-empty");

    // create a second client and try to delete the queue
    {
        Client secondClient;
        secondClient.connectToHost();
        QVERIFY(waitForSignal(&secondClient, SIGNAL(connected())));
        Queue *testDeleteQueue = secondClient.createQueue("test-remove-if-empty");
        testDeleteQueue->declare(Queue::Passive);
        QVERIFY(waitForSignal(testDeleteQueue, SIGNAL(declared())));

        testDeleteQueue->remove(Queue::roIfEmpty);
        QVERIFY(waitForSignal(testDeleteQueue, SIGNAL(error(QAMQP::Error))));
        QCOMPARE(testDeleteQueue->error(), QAMQP::PreconditionFailedError);
        QVERIFY(!testDeleteQueue->errorString().isEmpty());

        secondClient.disconnectFromHost();
        QVERIFY(waitForSignal(&secondClient, SIGNAL(disconnected())));
    }

    // clean up queue
    queue->remove(Queue::roForce);
    QVERIFY(waitForSignal(queue, SIGNAL(removed())));
}

void tst_QAMQPQueue::unbind()
{
    Queue *queue = client->createQueue("test-unbind");
    queue->declare();
    QVERIFY(waitForSignal(queue, SIGNAL(declared())));
    QVERIFY(queue->consume());   // required because AutoDelete will not delete if
                                 // there was never a consumer
    QVERIFY(waitForSignal(queue, SIGNAL(consuming(QString))));

    queue->bind("amq.topic", "routingKey");
    QVERIFY(waitForSignal(queue, SIGNAL(bound())));
    queue->unbind("amq.topic", "routingKey");
    QVERIFY(waitForSignal(queue, SIGNAL(unbound())));
}

void tst_QAMQPQueue::purge()
{
    Queue *queue = client->createQueue("test-purge");
    queue->declare(Queue::Durable);
    QVERIFY(waitForSignal(queue, SIGNAL(declared())));
    Exchange *defaultExchange = client->createExchange();
    defaultExchange->publish("first message", "test-purge");
    defaultExchange->publish("second message", "test-purge");
    defaultExchange->publish("third message", "test-purge");

    // create second client to listen to messages and attempt purge
    {
        Client secondClient;
        secondClient.connectToHost();
        QVERIFY(waitForSignal(&secondClient, SIGNAL(connected())));
        Queue *testPurgeQueue = secondClient.createQueue("test-purge");
        testPurgeQueue->declare(Queue::Passive);
        QVERIFY(waitForSignal(testPurgeQueue, SIGNAL(declared())));

        QSignalSpy spy(testPurgeQueue, SIGNAL(purged(int)));
        testPurgeQueue->purge();
        QVERIFY(waitForSignal(testPurgeQueue, SIGNAL(purged(int))));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(testPurgeQueue->size(), 0);
        QList<QVariant> arguments = spy.takeFirst();
        QCOMPARE(arguments.at(0).toInt(), 3);

        secondClient.disconnectFromHost();
        QVERIFY(waitForSignal(&secondClient, SIGNAL(disconnected())));
    }

    // clean up queue
    queue->remove(Queue::roForce);
    QVERIFY(waitForSignal(queue, SIGNAL(removed())));
}

void tst_QAMQPQueue::canOnlyStartConsumingOnce()
{
    Queue *queue = client->createQueue("test-single-consumer");
    queue->declare();
    QVERIFY(waitForSignal(queue, SIGNAL(declared())));
    QVERIFY(queue->consume());
    QSignalSpy spy(queue, SIGNAL(consuming(QString)));
    QVERIFY(waitForSignal(queue, SIGNAL(consuming(QString))));
    QVERIFY(queue->isConsuming());
    QVERIFY(!spy.isEmpty());
    QList<QVariant> arguments = spy.takeFirst();
    QCOMPARE(arguments.at(0).toString(), queue->consumerTag());
    QCOMPARE(queue->consume(), false);
}

void tst_QAMQPQueue::cancel()
{
    Queue *queue = client->createQueue("test-cancel");
    queue->declare();
    QVERIFY(waitForSignal(queue, SIGNAL(declared())));
    QVERIFY(queue->consume());
    QSignalSpy spy(queue, SIGNAL(consuming(QString)));
    QVERIFY(waitForSignal(queue, SIGNAL(consuming(QString))));
    QVERIFY(queue->isConsuming());
    QVERIFY(!spy.isEmpty());
    QList<QVariant> arguments = spy.takeFirst();
    QCOMPARE(arguments.at(0).toString(), queue->consumerTag());

    QString consumerTag = queue->consumerTag();
    QSignalSpy cancelSpy(queue, SIGNAL(cancelled(QString)));
    QVERIFY(queue->cancel());
    QVERIFY(waitForSignal(queue, SIGNAL(cancelled(QString))));
    QVERIFY(!cancelSpy.isEmpty());
    arguments = cancelSpy.takeFirst();
    QCOMPARE(arguments.at(0).toString(), consumerTag);
}

void tst_QAMQPQueue::invalidCancelBecauseNotConsuming()
{
    Queue *queue = client->createQueue("test-invalid-cancel-because-not-consuming");
    queue->declare();
    QVERIFY(waitForSignal(queue, SIGNAL(declared())));
    QCOMPARE(queue->cancel(), false);
}

void tst_QAMQPQueue::invalidCancelBecauseInvalidConsumerTag()
{
    Queue *queue = client->createQueue("test-invalid-cancel-because-invalid-consumer-tag");
    queue->declare();
    QVERIFY(waitForSignal(queue, SIGNAL(declared())));
    QVERIFY(queue->consume());
    QSignalSpy spy(queue, SIGNAL(consuming(QString)));
    QVERIFY(waitForSignal(queue, SIGNAL(consuming(QString))));
    QVERIFY(queue->isConsuming());
    QVERIFY(!spy.isEmpty());
    QList<QVariant> arguments = spy.takeFirst();
    QCOMPARE(arguments.at(0).toString(), queue->consumerTag());
    queue->setConsumerTag(QString());
    QCOMPARE(queue->cancel(), false);
}

QTEST_MAIN(tst_QAMQPQueue)
#include "tst_qamqpqueue.moc"
