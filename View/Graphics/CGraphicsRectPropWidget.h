/*
 * Copyright (C) 2021 Ikomia SAS
 * Contact: https://www.ikomia.com
 *
 * This file is part of the IkomiaStudio software.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef CGRAPHICSRECTPROPWIDGET_H
#define CGRAPHICSRECTPROPWIDGET_H

#include <QWidget>
#include "Widgets/CColorPushButton.h"

struct CGraphicsRectProperty;

class CGraphicsRectPropWidget : public QWidget
{
    Q_OBJECT

    public:

        explicit CGraphicsRectPropWidget(QWidget *parent = nullptr);

        void            setProperties(CGraphicsRectProperty *pProp);

        void            propertyChanged();

    private:

        void            initLayout();
        void            initConnections();

        void            updateUI();

    private:

        CColorPushButton*       m_pPenColorBtn = nullptr;
        CColorPushButton*       m_pBrushColorBtn = nullptr;
        QSpinBox*               m_pSpinSize = nullptr;
        QSpinBox*               m_pSpinOpacity = nullptr;
        QLineEdit*              m_pEditCategory = nullptr;
        CGraphicsRectProperty*   m_pProperty = nullptr;
};

#endif // CGRAPHICSRECTPROPWIDGET_H
