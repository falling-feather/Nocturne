#pragma once

#include <QImage>
#include <QStringList>
#include <QTextEdit>

class QMimeData;

class NoteEditor final : public QTextEdit
{
    Q_OBJECT

public:
    explicit NoteEditor(QWidget* parent = nullptr);

signals:
    void imagePasted(const QImage& image);
    void imageFilesDropped(const QStringList& files);

protected:
    bool canInsertFromMimeData(const QMimeData* source) const override;
    void insertFromMimeData(const QMimeData* source) override;
};

