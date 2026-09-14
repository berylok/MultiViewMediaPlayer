#ifndef VIDEOMETAINFO_H
#define VIDEOMETAINFO_H

#include <QString>
#include <QMetaType>

struct VideoMetaInfo {
    int     width = 0;
    int     height = 0;
    QString codec;
    double  fps = 0.0;
    qint64  bitrate = 0;
};

Q_DECLARE_METATYPE(VideoMetaInfo)

#endif // VIDEOMETAINFO_H
