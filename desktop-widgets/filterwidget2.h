#ifndef FILTERWIDGET_2_H
#define FILTERWIDGET_2_H

#include <QWidget>
#include <QHideEvent>
#include <QShowEvent>

#include <memory>

#include "ui_filterwidget2.h"
#include "qt-models/filtermodels.h"

namespace Ui {
	class FilterWidget2;
}

class FilterWidget2 : public QWidget {
	Q_OBJECT

public:
	explicit FilterWidget2(QWidget *parent = 0);
	void updateFilter();
protected:
	void hideEvent(QHideEvent *event) override;
	void showEvent(QShowEvent *event) override;

signals:
	void filterDataChanged(const FilterData& data);

public slots:
	void updatePlanned(int value);
	void updateLogged(int value);



private:
	std::unique_ptr<Ui::FilterWidget2> ui;
	FilterData filterData;
};

#endif
