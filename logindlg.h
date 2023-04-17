#pragma once

#include <qdialog.h>
#include <qlineedit.h>

class cLoginDlg : public QDialog
{
Q_OBJECT
public:
	cLoginDlg(QWidget *parent);
	~cLoginDlg();
public:
	QLineEdit *hostEdit;
	QLineEdit *portEdit;
	QLineEdit *nickEdit;
}
