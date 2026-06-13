#ifndef PXVIEW_PV_INTERFACE_ICONTEXTAWARE_H
#define PXVIEW_PV_INTERFACE_ICONTEXTAWARE_H

namespace pv {

class TabContext;

class IContextAware
{
public:
    virtual ~IContextAware() {}
    virtual void bind_context(TabContext *ctx) = 0;
    virtual void unbind_context() = 0;
};

} // namespace pv

#endif
