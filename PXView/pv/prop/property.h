/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */


#ifndef PXVIEW_PV_PROP_PROPERTY_H
#define PXVIEW_PV_PROP_PROPERTY_H

#include <glib.h>

#include <functional>

#include <QString>
#include <QWidget>

class QWidget;

namespace pv {
namespace prop {

class Property : public QObject
{
    Q_OBJECT;

public:
	using Getter = std::function<GVariant* ()>;
	using Setter = std::function<void (GVariant*)>;

protected:
    Property(QString name, QString label, Getter getter, Setter setter);

public:
    const QString& name();
    const QString& label();

    virtual ~Property();

    /* Build the editor widget. Subclasses implement the actual widget creation.
     * This is protected to prevent callers from using the ambiguous bool
     * parameter directly — use get_widget_live() or get_widget_deferred()
     * instead, which make the commit behavior explicit at the call site. */
protected:
    virtual QWidget* get_widget(QWidget *parent,
        bool auto_commit) = 0;
public:
    /* Live widget: commits on every user interaction (e.g. dropdown change,
     * spinbox valueChanged). Use for dock panels where each change should
     * immediately write to the driver. */
    QWidget* get_widget_live(QWidget *parent) {
        return get_widget(parent, true);
    }
    /* Deferred widget: does NOT auto-commit. Caller must invoke commit()
     * explicitly (e.g. dialog OK button). Use for modal dialogs that batch
     * all changes on confirmation. */
    QWidget* get_widget_deferred(QWidget *parent) {
        return get_widget(parent, false);
    }

	virtual bool labeled_widget();

    virtual GVariant* get_value();
    virtual void set_value(GVariant *value) { (void)value; }

    virtual void commit() = 0;

signals:
    void committed();

protected:
	const Getter _getter;
	const Setter _setter;

private:
    QString _name;
    QString _label;
};

} // prop
} // pv

#endif // PXVIEW_PV_PROP_PROPERTY_H
