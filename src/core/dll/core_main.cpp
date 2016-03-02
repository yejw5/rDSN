/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Microsoft Corporation
 * 
 * -=- Robust Distributed System Nucleus (rDSN) -=- 
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * Description:
 *     dsn (layer 1) initialization
 *
 * Revision history:
 *     July, 2015, @imzhenyu (Zhenyu Guo), first version
 *     xxxx-xx-xx, author, fix bug about xxx
 */


# include <dsn/service_api_c.h>
# include <dsn/internal/ports.h>

# include <dsn/tool/simulator.h>
# include <dsn/tool/nativerun.h>
# include <dsn/tool/fastrun.h>
# include <dsn/toollet/tracer.h>
# include <dsn/toollet/profiler.h>
# include <dsn/toollet/fault_injector.h>

# include <dsn/tool/providers.common.h>
# include <dsn/tool/providers.hpc.h>
# include <dsn/tool/nfs_node_simple.h>
# include <dsn/internal/singleton.h>

# include <dsn/dist/dist.providers.common.h>

//# include <dsn/thrift_helper.h>

# ifdef __TITLE__
# undef __TITLE__
# endif
# define __TITLE__ "core.main"

void dsn_core_init()
{
    // register all providers
    dsn::tools::register_common_providers();
    dsn::tools::register_hpc_providers();
    dsn::tools::register_component_provider< ::dsn::service::nfs_node_simple>("dsn::service::nfs_node_simple");

    //dsn::tools::register_component_provider<dsn::thrift_binary_message_parser>("thrift");

    // register all possible tools and toollets
    dsn::tools::register_tool<dsn::tools::nativerun>("nativerun");
    dsn::tools::register_tool<dsn::tools::fastrun>("fastrun");
    dsn::tools::register_tool<dsn::tools::simulator>("simulator");
    dsn::tools::register_toollet<dsn::tools::tracer>("tracer");
    dsn::tools::register_toollet<dsn::tools::profiler>("profiler");
    dsn::tools::register_toollet<dsn::tools::fault_injector>("fault_injector");

    // register useful distributed framework providers
    dsn::dist::register_common_providers();
}

//
// global checker implementation
//
void sys_init_for_add_global_checker(::dsn::configuration_ptr config);
class global_checker_store : public ::dsn::utils::singleton< global_checker_store >
{
public:
    struct global_checker
    {
        std::string        name;
        dsn_checker_create create;
        dsn_checker_apply  apply;
    };

public:
    global_checker_store()
    {
        ::dsn::tools::sys_init_after_app_created.put_back(
            sys_init_for_add_global_checker,
            "checkers.install"
            );
    }

    std::list<global_checker> checkers;
};

void sys_init_for_add_global_checker(::dsn::configuration_ptr config)
{
    auto t = dynamic_cast<dsn::tools::simulator*>(::dsn::tools::get_current_tool());
    if (t != nullptr)
    {
        for (auto& c : global_checker_store::instance().checkers)
        {
            t->add_checker(c.name.c_str(), c.create, c.apply);
        }
    }
}

DSN_API void dsn_register_app_checker(const char* name, dsn_checker_create create, dsn_checker_apply apply)
{
    global_checker_store::global_checker ck;
    ck.name = name;
    ck.create = create;
    ck.apply = apply;

    global_checker_store::instance().checkers.push_back(ck);
}
