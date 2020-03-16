#include <utils.h>
#include <QVector2D>
#include "connector.h"
#include "../scene.h"
#include "wirenet.h"
#include "wire.h"
#include "label.h"
#include "itemfactory.h"

using namespace QSchematic;

WireNet::WireNet(QObject* parent) :
    QObject(parent), _scene(nullptr)
{
    // Label
    _label = std::make_shared<Label>();
    _label->setPos(0, 0);
    _label->setVisible(false);
    connect(_label.get(), &Label::highlightChanged, this, &WireNet::labelHighlightChanged);
    connect(_label.get(), &Label::moved, this, [=] { updateLabelPos(); });
}

WireNet::~WireNet()
{
    if (_label) {
        _label->setParentItem(nullptr);
        dissociate_item(_label);
    }
}

gpds::container WireNet::to_container() const
{
    // Wires
    gpds::container wiresContainer;
    for (const auto& wire : m_wires) {
        if (auto wire_net = std::dynamic_pointer_cast<Wire>(wire.lock()))
        {
            wiresContainer.add_value("wire", wire_net->to_container());
        }
    }

    // Root
    gpds::container root;
    root.add_value("name", m_name.toStdString() );
    // The coordinates of the label need to be in the scene space
    if (_label->parentItem()) {
        _label->moveBy(QVector2D(_label->parentItem()->pos()));
    }
    root.add_value("label", _label->to_container());
    // Move the label back to the correct position
    if (_label->parentItem()) {
        _label->moveBy(-QVector2D(_label->parentItem()->pos()));
    }
    root.add_value("wires", wiresContainer);

    return root;
}

void WireNet::from_container(const gpds::container& container)
{
    Q_ASSERT(_scene);
    // Root
    set_name(QString::fromStdString(container.get_value<std::string>("name").value_or("")));

    // Label
    if (auto labelContainer = container.get_value<gpds::container*>("label").value_or(nullptr)) {
        _label->from_container(*labelContainer);
    }

    // Wires
    {
        const gpds::container& wiresContainer = *container.get_value<gpds::container*>("wires").value();
        for (const gpds::container* wireContainer : wiresContainer.get_values<gpds::container*>("wire")) {
            Q_ASSERT(wireContainer);

            auto newWire = ItemFactory::instance().from_container(*wireContainer);
            auto sharedNewWire = std::dynamic_pointer_cast<Wire>( newWire );
            if (!sharedNewWire) {
                continue;
            }
            sharedNewWire->from_container(*wireContainer);
            addWire(sharedNewWire);
            if (not _scene) {
                qCritical("WireNet::from_container(): The scene has not been set.");
                return;
            }
            _scene->addItem(sharedNewWire);
        }
    }
}

bool WireNet::addWire(const std::shared_ptr<wire>& wire)
{
    if (not net::addWire(wire)) {
        return false;
    }

    if (auto wire_net = std::dynamic_pointer_cast<Wire>(wire))
    {
        // Connect signals
        connect(wire_net.get(), &Wire::pointMoved, this, &WireNet::wirePointMoved);
        connect(wire_net.get(), &Wire::highlightChanged, this, &WireNet::wireHighlightChanged);
        connect(wire_net.get(), &Wire::toggleLabelRequested, this, &WireNet::toggleLabel);
        connect(wire_net.get(), &Wire::moved, this, [=] { updateLabelPos(); });
    }


    updateLabelPos(true);

    return true;
}

bool WireNet::removeWire(const std::shared_ptr<wire> wire)
{
    if (auto wire_net = std::dynamic_pointer_cast<Wire>(wire)) {
        disconnect(wire_net.get(), nullptr, this, nullptr);
    }
    net::removeWire(wire);
    updateLabelPos(true);

    return true;
}

void WireNet::simplify()
{
    for (auto& wire : m_wires) {
        wire.lock()->simplify();
    }
}

void WireNet::set_name(const QString& name)
{
    net::set_name(name);

    _label->setText(m_name);
    _label->setVisible(!m_name.isEmpty());
    updateLabelPos(true);
}

void WireNet::setHighlighted(bool highlighted)
{
    // Wires
    for (auto& wire : m_wires) {
        if (wire.expired()) {
            continue;
        }

        if (auto wire_item = std::dynamic_pointer_cast<Wire>(wire.lock())) {
            wire_item->setHighlighted(highlighted);
        }
    }

    // Label
    _label->setHighlighted(highlighted);

    highlight_global_net(highlighted);
}

/**
 * Returns a list of all the nets that are in the same global net as the given net
 */
QList<std::shared_ptr<WireNet>> WireNet::nets() const
{
    QList<std::shared_ptr<WireNet>> list;

    for (auto& net : m_manager->nets()) {
        if (!net) {
            continue;
        }

        if (net->name().isEmpty()) {
            continue;
        }

        if (QString::compare(net->name(), name(), Qt::CaseInsensitive) == 0) {
            if (auto otherNet = std::dynamic_pointer_cast<WireNet>(net)) {
                list.append(otherNet);
            }
        }
    }

    return list;
}

void WireNet::highlight_global_net(bool highlighted)
{
    // Highlight all wire nets that are part of this net
    for (auto& otherWireNet : nets()) {
        if (otherWireNet.get() == this) {
            continue;
        }

        otherWireNet->blockSignals(true);
        otherWireNet->setHighlighted(highlighted);
        otherWireNet->blockSignals(false);
    }
}

void WireNet::setScene(Scene* scene)
{
    _scene = scene;
}

QList<line> WireNet::lineSegments() const
{
    QList<line> list;

    for (const auto& wire : m_wires) {
        if (wire.expired()) {
            continue;
        }

        list.append(wire.lock()->line_segments());
    }

    return list;
}

QList<QPointF> WireNet::points() const
{
    QList<QPointF> list;

    for (const auto& wire : m_wires) {
        for (const auto& point : wire.lock()->points()) {
            list.append(point.toPointF());
        }
    }

    return list;
}

std::shared_ptr<Label> WireNet::label()
{
    return _label;
}

void WireNet::wirePointMoved(Wire& wire, const point& point)
{
    updateLabelPos();
}

/**
 * Update the label's connection point and its parent if updateParent is true.
 */
void WireNet::updateLabelPos(bool updateParent) const
{
    // Ignore if the label is not visible
    if (not _label->isVisible()) {
        return;
    }
    // Find closest point
    QPointF labelPos = _label->textRect().center() + _label->scenePos();
    QPointF closestPoint;
    std::shared_ptr<wire> closestWire;
    for (const auto& wire : m_wires) {
        std::shared_ptr<wire_system::wire> spWire = wire.lock();
        for (const auto& segment: spWire->line_segments()) {
            // Find closest point on segment
            QPointF p = Utils::pointOnLineClosestToPoint(segment.p1(), segment.p2(), labelPos);
            float distance1 = QVector2D(labelPos - closestPoint).lengthSquared();
            float distance2 = QVector2D(labelPos - p).lengthSquared();
            if (closestPoint.isNull() or distance1 > distance2) {
                closestPoint = p;
                closestWire = spWire;
            }
        }
    }
    // If there are no wires left in the net it will be hidden anyway
    if (not closestWire) {
        return;
    }
    // Update the parent if requested
    auto closestWireItem = std::dynamic_pointer_cast<Wire>(closestWire);
    if (updateParent and _label->parentItem() != closestWireItem.get()) {
        _label->setParentItem(closestWireItem.get());
        _label->setPos(labelPos - _label->textRect().center() - closestWireItem->scenePos());
    }
    // Update the connection point
    if (_label->parentItem()) {
        _label->setConnectionPoint(closestPoint - _label->parentItem()->pos());
    } else {
        _label->setConnectionPoint(closestPoint);
    }

}

void WireNet::labelHighlightChanged(const Item& item, bool highlighted)
{
    Q_UNUSED(item)

    setHighlighted(highlighted);
}

void WireNet::wireHighlightChanged(const Item& item, bool highlighted)
{
    Q_UNUSED(item)

    setHighlighted(highlighted);
}

void WireNet::toggleLabel()
{
    _label->setVisible(not _label->text().isEmpty() and not _label->isVisible());
    updateLabelPos(true);
}
