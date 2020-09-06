#include <QList>
#include <QDebug>
#include <string.h>
#include <math.h>

#include "temporaryexposurekey.h"
#include "contrac.pb.h"
#include "zipistreambuffer.h"
#include "contactinterval.h"
#include "exposuresummary.h"
#include "settings.h"

#include "exposurenotification.h"
#include "exposurenotification_p.h"

#define MAX_DIAGNOSIS_KEYS (1024)
// The spaces are intentional, to pad to 16 bytes
#define EXPORT_BIN_HEADER QLatin1String("EK Export v1    ")

// In milliseconds
#define CONTIGUOUS_PERIOD_THRESHOLD (10 * 60 * 1000)

namespace {

inline qint32 checkLowerThreshold(qint32 thresholds[8], QList<qint32> scores, qint32 value)
{
    qint8  pos = 7;
    while ((pos > 0) && (value < thresholds[pos - 1])) {
        --pos;
    }

    return scores[pos];
}

inline qint32 checkGreaterThreshold(qint32 thresholds[8], QList<qint32> scores, qint32 value)
{
    qint8 pos = 7;
    while ((pos > 0) && (value > thresholds[pos - 1])) {
        --pos;
    }

    return scores[pos];
}

template<class T>
inline T clamp(T value, T min, T max)
{
    if (value < min) {
        value = min;
    }
    else {
        if (value > max) {
            value = max;
        }
    }
    return value;
}

inline qint32 attenuationCalculation(qint16 rssiMeasured, qint8 rssiCorrection, qint8 txPower)
{
    // Attenuation = TX_power - (RSSI_measured + RSSI_correction)
    // See https://developers.google.com/android/exposure-notifications/ble-attenuation-overview
    qint32 attenuation;

    attenuation = txPower - (rssiMeasured + rssiCorrection);

    return attenuation;
}

inline bool compareIntervals(RpiDataItem const &rpi1, RpiDataItem const & rpi2) {
    return rpi1.m_interval < rpi2.m_interval;
}

}

ExposureNotificationPrivate::ExposureNotificationPrivate(ExposureNotification *q)
    : QObject(q)
    , q_ptr(q)
    , m_status(ExposureNotification::Success)
{
    qDebug() << "Contrac";
    m_contrac = new Contrac(q);
    qDebug() << "Controller";
    m_controller = new Controller(q);
    qDebug() << "Scanner";
    m_scanner = new BleScanner(q);
    qDebug() << "Contacts";
    m_contacts = new ContactStorage(m_contrac);
    qDebug() << "Timer";
    // Update at least every ten minutes when active
    m_intervalUpdate.setInterval(5 * 1000);
    m_intervalUpdate.setSingleShot(false);

    qDebug() << "Load";
    QByteArray tk = Settings::getInstance().tracingKey();
    if (tk.isEmpty()) {
        qDebug() << "Generating new tracing key";
        m_contrac->generateTracingKey();
        tk = m_contrac->tk();
        Settings::getInstance().setTracingKey(tk);
    }
    else {
        qDebug() << "Using existing tracing key";
        m_contrac->setTk(tk);
    }
    m_contrac->updateKeys();

    qDebug() << "Connections";
    q_ptr->connect(m_contrac, &Contrac::rpiChanged, q_ptr, &ExposureNotification::onRpiChanged);
    q_ptr->connect(m_contrac, &Contrac::timeChanged, m_contacts, &ContactStorage::onTimeChanged);
    q_ptr->connect(&m_intervalUpdate, &QTimer::timeout, q_ptr, &ExposureNotification::intervalUpdate);
    q_ptr->connect(m_scanner, &BleScanner::beaconDiscovered, q_ptr, &ExposureNotification::beaconDiscovered);
    q_ptr->connect(m_scanner, &BleScanner::scanChanged, this, &ExposureNotificationPrivate::scanChanged);
    q_ptr->connect(m_scanner, &BleScanner::busyChanged, q_ptr, &ExposureNotification::isBusyChanged);
}

ExposureNotificationPrivate::~ExposureNotificationPrivate()
{
    q_ptr->disconnect(m_scanner, &BleScanner::beaconDiscovered, q_ptr, &ExposureNotification::beaconDiscovered);
    q_ptr->disconnect(&m_intervalUpdate, &QTimer::timeout, q_ptr, &ExposureNotification::intervalUpdate);
    q_ptr->disconnect(m_contrac, &Contrac::timeChanged, m_contacts, &ContactStorage::onTimeChanged);
    q_ptr->disconnect(m_contrac, &Contrac::rpiChanged, q_ptr, &ExposureNotification::onRpiChanged);
    q_ptr->disconnect(m_scanner, &BleScanner::scanChanged, this, &ExposureNotificationPrivate::scanChanged);
    q_ptr->disconnect(m_scanner, &BleScanner::busyChanged, q_ptr, &ExposureNotification::isBusyChanged);
}

ExposureNotification::ExposureNotification(QObject *parent)
    : QObject(parent)
    , d_ptr(new ExposureNotificationPrivate(this))
{
}

ExposureNotification::~ExposureNotification()
{
    Q_D(ExposureNotification);

    delete d;
}

bool ExposureNotification::isEnabled() const
{
    Q_D(const ExposureNotification);

    return d->m_scanner->scan();
}

bool ExposureNotification::isBusy() const
{
    Q_D(const ExposureNotification);

    return d->m_scanner->busy();
}

quint32 ExposureNotification::maxDiagnosisKeys() const
{
    return MAX_DIAGNOSIS_KEYS;
}

ExposureNotification::Status ExposureNotification::status() const
{
    Q_D(const ExposureNotification);

    return d->m_status;
}

void ExposureNotification::start()
{
    Q_D(ExposureNotification);

    d->m_contrac->updateKeys();

    if (!d->m_scanner->scan()) {
        d->m_scanner->setScan(true);
        d->m_controller->registerAdvert();
    }
    d->m_intervalUpdate.start();
}

void ExposureNotification::stop()
{
    Q_D(ExposureNotification);

    d->m_intervalUpdate.stop();
    if (d->m_scanner->scan()) {
        d->m_scanner->setScan(false);
        d->m_controller->unRegisterAdvert();
    }
}

// Return the last 14 days of history
QList<TemporaryExposureKey> ExposureNotification::getTemporaryExposureKeyHistory()
{
    Q_D(ExposureNotification);

    QList<TemporaryExposureKey> keys;
    quint32 today;
    quint32 day;

    today = d->m_contrac->dayNumber();
    day = today < 14 ? 0 : today - 14;
    while (day < today) {
        QByteArray dtk;
        dtk = d->m_contrac->dailyTracingKey(d->m_contrac->tk(), day);
        TemporaryExposureKey key(this);
        key.setKeyData(dtk);
        key.setRollingStartNumber(day * 144);
        key.setRollingPeriod(144);
        // TODO: Figure out where this is supposed to come from
        key.setTransmissionRiskLevel(TemporaryExposureKey::RiskLevelMedium);
        keys.append(key);
        ++day;
    }

    return keys;
}

void ExposureNotification::provideDiagnosisKeys(QVector<QString> const &keyFiles, ExposureConfiguration const &configuration, QString token)
{
    Q_D(ExposureNotification);

    bool result;
    diagnosis::TemporaryExposureKeyExport keyExport;
    int pos;
    QList<DiagnosisKey> diagnosisKeys[DAYS_TO_STORE];
    QList<ExposureInformation> exposureInfoList;

    d->m_contrac->updateKeys();

    // Values will accumulate
    // We assume the same keyFiles won't be provided more than once
    if (d->m_exposures.contains(token)) {
        exposureInfoList = d->m_exposures.value(token);
    }

    for (QString const &file : keyFiles) {
        // TODO: Check region
        // TODO: Check batch numbering
        // TODO: Check start/end timestamps
        result = ExposureNotificationPrivate::loadDiagnosisKeys(file, &keyExport);
        if (result) {
            for(pos = 0; pos < keyExport.keys_size(); ++pos) {
                diagnosis::TemporaryExposureKey const &key = keyExport.keys(pos);
                QByteArray dtk(key.key_data().data(), static_cast<int>(key.key_data().length()));
                DiagnosisKey diagnosisKey(dtk, static_cast<DiagnosisKey::RiskLevel>(key.transmission_risk_level()), static_cast<quint32>(key.rolling_start_interval_number()), static_cast<quint32>(key.rolling_period()));
                qint64 day = static_cast<quint32>(key.rolling_start_interval_number() / 144) - d->m_contrac->dayNumber();
                if (day >= 0 && day < DAYS_TO_STORE) {
                    diagnosisKeys[day].append(diagnosisKey);
                }

            }
        }
        else {
            qDebug() << "Error loading diagnosis key file: " << file;
        }
    }

    for (quint32 day = 0; day < DAYS_TO_STORE; ++day) {
        if (diagnosisKeys[day].length() > 0) {
            quint32 const dayNumber = d->m_contrac->dayNumber() + day;
            QList<ContactMatch> matches = d->m_contacts->findDtkMatches(dayNumber, diagnosisKeys[day]);

            qint32 days_ago = static_cast<qint32>(dayNumber) - static_cast<qint32>(d->m_contrac->dayNumber());
            exposureInfoList.append(d->aggregateExposureData(dayNumber, configuration, matches, days_ago));
        }
    }

    // Replace the previous values with the accumulated total
    d->m_exposures.insert(token, exposureInfoList);
}

QList<ExposureInformation> ExposureNotificationPrivate::aggregateExposureData(quint32 dayNumber, ExposureConfiguration const &configuration, QList<ContactMatch> matches, qint32 const days_ago)
{
    QList<ExposureInformation> exposures;
    qint32 attenuationThreshold[2];
    attenuationThreshold[0] = configuration.durationAtAttenuationThresholds()[0];
    attenuationThreshold[1] = configuration.durationAtAttenuationThresholds()[1];

    qint32 transmissionRisk;
    qint32 totalDuration;
    //qint32 days_ago;
    qint32 attenuationValue;

    Metadata metadata;
    qint8 rssiCorrection;
    qint8 txPower;
    qint32 attenuation;

    rssiCorrection = Settings::getInstance().rssiCorrection();

    for (ContactMatch match : matches) {
        metadata.setDtk(match.m_dtk->m_dtk);

        // The aggregation process assumes that rpi intervals are increasing
        // So order the RPIs if necessary to ensure this
        // This is just for safety, by far the most likely situation is that they'll already be ordered
        if (!std::is_sorted(match.m_rpis.begin(), match.m_rpis.end(), compareIntervals)) {
            qDebug() << "Reordering RPIs to have increasing interval values";
            std::sort(match.m_rpis.begin(), match.m_rpis.end(), compareIntervals);
        }

        int rpi_pos = 0;
        qDebug() << "Aggregating data for rpi matches:" << match.m_rpis.size();
        while (rpi_pos < match.m_rpis.size()) {
            ExposureInformation exposure;

            quint64 const dateMillisSinceEpoch = dayNumber * 24 * 60 * 60 * 1000;
            exposure.setDateMillisSinceEpoch(dateMillisSinceEpoch);
            transmissionRisk = static_cast<qint32>(match.m_dtk->m_transmissionRiskLevel);
            exposure.setTransmissionRiskLevel(transmissionRisk);

            qint64 interval = match.m_rpis[rpi_pos].m_interval;
            totalDuration = 0;
            qint32 totalRiskScore;
            qint32 attenuationDurations[3] = {0, 0, 0};
            qint64 attenuationSum = 0;
            while (rpi_pos < match.m_rpis.size() && (match.m_rpis[rpi_pos].m_interval < interval + (CONTIGUOUS_PERIOD_THRESHOLD / CTINTERVAL_DURATION) + 1)) {
                RpiDataItem const &rpi = match.m_rpis[rpi_pos];
                // The timeDelta value should be measured in minutes and rounded upwards
                qint64 timeDelta = qint64(ceil((qint64(1 + rpi.m_interval) - interval) / double(60 * 1000 / CTINTERVAL_DURATION)));
                // Make use of the txPower stored in the AEM
                metadata.setEncryptedMetadata(rpi.m_aem);
                metadata.setRpi(rpi.m_rpi);
                txPower = metadata.txPower();
                // The metadata must also be valid
                if (metadata.valid()) {
                    attenuation = attenuationCalculation(rpi.m_rssi, rssiCorrection, txPower);

                    // Attenuation durations in minutes
                    // From the specification:
                    // "Array of durations in minutes at certain radio signal attenuations."
                    if (attenuation < attenuationThreshold[0]) {
                        attenuationDurations[0] += timeDelta;
                    }
                    else {
                        if (attenuation < attenuationThreshold[1]) {
                            attenuationDurations[1] += timeDelta;
                        }
                        else {
                            attenuationDurations[2] += timeDelta;
                        }
                    }
                    attenuationSum += (attenuation * timeDelta);

                    interval = rpi.m_interval;
                    totalDuration += timeDelta;
                }
                else {
                    qDebug() << "Invalid metadata";
                }

                ++rpi_pos;
            }
            // Duration to the nearest 5 minutes
            // From the specification:
            // "Length of exposure in 5 minute increments, with a 30 minute maximum."
            qint32 durationMinutes = 5 * qint32(ceil(totalDuration / 5.0));
            if (durationMinutes > 30) {
                durationMinutes = 30;
            }
            exposure.setDurationMinutes(durationMinutes);
            QList<qint32> attenuationDurationList;
            attenuationDurationList.append(attenuationDurations[0]);
            attenuationDurationList.append(attenuationDurations[1]);
            attenuationDurationList.append(attenuationDurations[2]);
            exposure.setAttenuationDurations(attenuationDurationList);
            attenuationValue = totalDuration > 0 ? qint32(attenuationSum / totalDuration) : 0;
            exposure.setAttenuationValue(attenuationValue);

            // We no longer calculate days_ago to allow unit tests to work correctly
            //days_ago = static_cast<qint32>(dayNumber) - static_cast<qint32>(m_contrac->dayNumber());
            totalRiskScore = calculateRiskScore(configuration, transmissionRisk, totalDuration, days_ago, attenuationValue);

            exposure.setTotalRiskScore(totalRiskScore);

            exposures.append(exposure);
        }
    }

    return exposures;
}

qint32 ExposureNotificationPrivate::calculateRiskScore(ExposureConfiguration const &configuration, qint32 transmissionRisk, qint32 duration, qint32 days_ago, qint32 attenuationValue)
{
    qint32 riskScore;
    qint32 attenuationScore;
    qint32 daysSinceLastExposureScore;
    qint32 durationScore;
    qint32 transmissionRiskScore;
    qint8 pos;

    qint32 attenuationThresholds[8] = {73, 63, 51, 33, 27, 15, 10};
    qint32 daysThresholds[8] = {14, 12, 10, 8, 6, 4, 2};
    qint32 durationThreshold[8] = {1, 6, 11, 16, 21, 26, 31};

    attenuationScore = checkGreaterThreshold(attenuationThresholds, configuration.attenuationScores(), attenuationValue);
    daysSinceLastExposureScore = checkGreaterThreshold(daysThresholds, configuration.daysSinceLastExposureScores(), days_ago);
    durationScore = checkLowerThreshold(durationThreshold, configuration.durationScores(), duration);

    pos = clamp(static_cast<qint8>(transmissionRisk), static_cast<qint8>(0), static_cast<qint8>(7));
    transmissionRiskScore = configuration.transmissionRiskScores()[pos];

    riskScore = attenuationScore * daysSinceLastExposureScore * durationScore * transmissionRiskScore;

    return riskScore;
}

bool ExposureNotificationPrivate::loadDiagnosisKeys(QString const &keyFile, diagnosis::TemporaryExposureKeyExport *keyExport)
{
    QuaZip quazip(keyFile);
    bool result = true;
    QStringList files;

    GOOGLE_PROTOBUF_VERIFY_VERSION;

    result = (keyExport != nullptr);

    if (result) {
        result = quazip.open(QuaZip::mdUnzip, nullptr);
    }

    if (result) {
        // The zip archive must contain exactly two entries
        files = quazip.getFileNameList();
        if ((files.size() != 2) || !files.contains("export.sig") || !files.contains("export.bin")) {
            result = false;
        }
    }

    //TODO: Check signature file

    if (result) {
        ZipIStreamBuffer streambuf(quazip, "export.bin");
        qDebug() << "Stream is good: " << streambuf.success();
        std::istream stream(&streambuf);
        QByteArray header(16, '\0');
        stream.read(header.data(), 16);

        result = (header == QString(EXPORT_BIN_HEADER));

        if (result) {
            result = keyExport->ParseFromIstream(&stream);
        }
    }

    if (result) {
        qDebug() << "Diagnosis file region: " << keyExport->region().data();
        qDebug() << "Diagnosis file start timestamp: " << keyExport->start_timestamp();
        qDebug() << "Diagnosis file end timestamp: " << keyExport->end_timestamp();
        qDebug() << "Diagnosis file keys: " << keyExport->keys().size();
        qDebug() << "Diagnosis file verification key: " << keyExport->signature_infos().size();
    }
    else {
        qDebug() << "TemporaryExposureKeyExport proto file failed to load";
    }

    return result;
}

void ExposureNotificationPrivate::scanChanged()
{
    Q_Q(ExposureNotification);

    Settings &settings = Settings::getInstance();
    bool scan = m_scanner->scan();
    settings.setEnabled(scan);

    emit q->isEnabledChanged();
}

ExposureSummary ExposureNotification::getExposureSummary(QString const &token) const
{
    Q_D(const ExposureNotification);

    QList<ExposureInformation> exposureInfoList;
    ExposureSummary summary;
    quint64 mostRecent;
    quint32 day;
    qint32 maxRiskScore;
    qint32 summationRiskScore;
    QList<qint32> attenuationDurations = QList<qint32>({0, 0, 0});

    if (d->m_exposures.contains(token)) {
        exposureInfoList = d->m_exposures.value(token);

        summationRiskScore = 0;
        mostRecent = 0;
        maxRiskScore = 0;
        for (ExposureInformation const &exposure : exposureInfoList) {
            if (exposure.dateMillisSinceEpoch() > mostRecent) {
                mostRecent = exposure.dateMillisSinceEpoch();
            }
            if (exposure.totalRiskScore() > maxRiskScore) {
                maxRiskScore = exposure.totalRiskScore();
            }
            attenuationDurations[0] += exposure.attenuationDurations()[0];
            attenuationDurations[1] += exposure.attenuationDurations()[1];
            attenuationDurations[2] += exposure.attenuationDurations()[2];

            summationRiskScore += exposure.totalRiskScore();
        }
        day = static_cast<quint32>(mostRecent / (24 * 60 * 60 * 1000));
        day += 12;

        summary.setDaysSinceLastExposure(day);
        summary.setMatchedKeyCount(static_cast<quint32>(exposureInfoList.count()));
        summary.setMaximumRiskScore(maxRiskScore);
        summary.setAttenuationDurations(attenuationDurations);
        summary.setSummationRiskScore(summationRiskScore);
    }

    return summary;
}

QList<ExposureInformation> ExposureNotification::getExposureInformation(QString const &token) const
{
    Q_D(const ExposureNotification);

    return d->m_exposures.value(token, QList<ExposureInformation>());
}

quint32 ExposureNotification::getMaxDiagnosisKeys() const
{
    return maxDiagnosisKeys();
}

void ExposureNotification::resetAllData()
{
    Q_D(ExposureNotification);

    d->m_contrac->updateKeys();
    d->m_contrac->generateTracingKey();
    d->m_contrac->saveTracingKey();
    d->m_contacts->clearAllDataFiles();
}

void ExposureNotification::beaconDiscovered(const QString &, const QByteArray &data, qint16 rssi)
{
    Q_D(ExposureNotification);
    QByteArray rpi;
    QByteArray aem;
    qDebug() << "Beacon discovered";

    if (data.length() == (RPI_SIZE + AEM_SIZE)) {
        rpi = data.mid(0, RPI_SIZE);
        qDebug() << "RPI:" << rpi.toHex();
        aem = data.mid(RPI_SIZE, AEM_SIZE);
        qDebug() << "AEM:" << aem.toHex();

        qint64 millisecondsSinceEpoch = QDateTime::currentDateTime().toMSecsSinceEpoch();

        ctinterval interval = millisecondsToCtInterval(millisecondsSinceEpoch);
        d->m_contrac->updateKeys();
        d->m_contacts->addContact(interval, rpi, aem, rssi);

        emit beaconReceived();
    }
    else {
        qDebug() << "Beacon has incorrect length: " << data.length();
    }
}

void ExposureNotification::intervalUpdate()
{
    Q_D(ExposureNotification);

    qDebug() << "intervalUpdate";
    d->m_contrac->updateKeys();
}

void ExposureNotification::onRpiChanged()
{
    Q_D(ExposureNotification);
    QByteArray dtk;
    QByteArray rpi;
    QByteArray metadata;
    qint8 txPower;

    qDebug() << "CONTRAC: onRpiChanged";
    dtk = d->m_contrac->dtk();
    rpi = d->m_contrac->rpi();
    txPower = Settings::getInstance().txPower();
    d->m_metadata.setDtk(dtk);
    d->m_metadata.setRpi(rpi);
    d->m_metadata.setTxPower(txPower);
    metadata = d->m_metadata.encryptedMetadata();
    if (!d->m_metadata.valid()) {
        qDebug() << "Metadata could not be created";
    }

    d->m_controller->setAdvertData(rpi, metadata);

    emit beaconSent();
}
