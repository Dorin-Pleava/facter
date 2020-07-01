#include <facter/ruby/ruby.hpp>
#include <facter/logging/logging.hpp>
#include <internal/ruby/module.hpp>
#include <internal/ruby/ruby_value.hpp>
#include <leatherman/ruby/api.hpp>
#include <leatherman/logging/logging.hpp>
#ifdef _WIN32
#include <internal/util/windows/wsa.hpp>
#endif

#include <numeric>

using namespace std;
using namespace facter::facts;
using namespace leatherman::ruby;

static const char load_puppet[] =
"require 'puppet'\n"
"Puppet.initialize_settings\n"
"unless $LOAD_PATH.include?(Puppet[:libdir])\n"
"  $LOAD_PATH << Puppet[:libdir]\n"
"end\n"
"Facter.reset\n"
"Facter.search_external([Puppet[:pluginfactdest]])\n"
"if Puppet.respond_to? :initialize_facts\n"
"  Puppet.initialize_facts\n"
"else\n"
"  Facter.add(:puppetversion) do\n"
"    setcode { Puppet.version.to_s }\n"
"  end\n"
"end\n";

// This struct redirects stdout to stderr in the ruby runtime for the
// duration of its lifetime. We use this to ensure that any custom
// facts writing to stdout during their initialization or execution
// won't corrupt json/yaml output from the facter executable.
struct RbStdoutGuard {
    VALUE old_stdout;
    api& ruby;

    RbStdoutGuard(api& ruby) :ruby(ruby) {
        LOG_DEBUG("Redirecting ruby's stdout to stderr");
        auto rb_stderr = ruby.rb_gv_get("$stderr");
        old_stdout = ruby.rb_gv_get("$stdout");
        ruby.rb_gv_set("$stdout", rb_stderr);
    }

    ~RbStdoutGuard() {
        LOG_DEBUG("Restoring Ruby's stdout");
        ruby.rb_gv_set("$stdout", old_stdout);
    }
};

namespace facter { namespace ruby {

    bool initialize(bool include_stack_trace)
    {
#ifdef FACTER_RUBY
        api::ruby_lib_location = FACTER_RUBY;
#endif
        try {
            auto& ruby = api::instance();
            ruby.initialize();
            ruby.include_stack_trace(include_stack_trace);
        } catch (runtime_error& ex) {
            LOG_WARNING("{1}: facts requiring Ruby will not be resolved.", ex.what());
            return false;
        }
        return true;
    }

    void load_custom_facts(collection& facts, bool initialize_puppet, bool redirect_stdout, vector<string> const& paths)
    {
#ifdef _WIN32
        // Initialize WSA before resolving custom facts. The Ruby runtime does this only when running
        // in a Ruby process, it leaves it up to us when embedding it. See
        // https://github.com/ruby/ruby/blob/v2_1_9/ruby.c#L2011-L2022 for comments.
        // The only piece we seem to need out of rb_w32_sysinit is WSAStartup.
        util::windows::wsa winsocket;

        // Disable stdout buffering while loading custom facts, similar to `stderr` in `init_stdhandle`
        // https://github.com/ruby/ruby/blob/9e41a75255d15765648279629fd3134cae076398/win32/win32.c#L2655
        // This is needed in a specific case:
        // - run facter from ruby with backticks
        // - have a custom fact executing external command with backticks
        // In this case, `\x00` character will be shown on stdout instead of fact output
        // We suppose that somwhere between ruby(`facter my_fact`)<->c(rb_load)<->ruby(Facter.add)<->c(rb_funcall_passing_block)<->ruby(`echo test`)
        // stdout gets the wchar end of string that will break it
        setvbuf(stdout, NULL, _IONBF, 0);
#endif
        api& ruby = api::instance();
        module mod(facts, {}, !initialize_puppet);
        if (initialize_puppet) {
            try {
                ruby.eval(load_puppet);
            } catch (exception& ex) {
                LOG_WARNING("Could not load puppet; some facts may be unavailable: {1}", ex.what());
            }
        }
        mod.search(paths);
        if (redirect_stdout) {
            // Redirect stdout->stderr for custom facts.
            RbStdoutGuard stdout_guard{ruby};
            mod.resolve_facts();
        } else {
            mod.resolve_facts();
        }
#ifdef _WIN32
        // Enable stdout line buffering (disabled due custom facts loading)
        setvbuf(stdout, NULL, _IOLBF, 0);
#endif
    }

    void load_custom_facts(collection& facts, vector<string> const& paths)
    {
        load_custom_facts(facts, false, false, paths);
    }

    void load_custom_facts(collection& facts, bool initialize_puppet, vector<string> const& paths)
    {
        load_custom_facts(facts, initialize_puppet, false, paths);
    }

    value const* lookup(value const* value, vector<string>::iterator segment, vector<string>::iterator end) {
        auto rb_value = dynamic_cast<ruby_value const*>(value);
        if (!rb_value) {
             return nullptr;
        }

        // Check for a cached lookup
        auto key = accumulate(segment, end, string{}, [](const string& a, const string& b) -> string {
                 if (b.find(".") != string::npos) {
                     return a+".\""+b+"\"";
                 } else {
                     return a+"."+b;
                 }
             });
        auto child_value = rb_value->child(key);
        if (child_value) {
            return child_value;
        }

        auto val = rb_value->value();  // now we're in ruby land
        api& ruby = api::instance();

        for (; segment != end; ++segment) {
            if (ruby.is_array(val)) {
                long index;
                try {
                    index = stol(*segment);
                } catch (logic_error&) {
                    LOG_DEBUG("cannot lookup an array element with \"{1}\": expected an integral value.", *segment);
                    return nullptr;
                }
                if (index < 0) {
                    LOG_DEBUG("cannot lookup an array element with \"{1}\": expected a non-negative value.", *segment);
                    return nullptr;
                }
                long length = ruby.array_len(val);
                if (0 == length) {
                    LOG_DEBUG("cannot lookup an array element with \"{1}\": the array is empty.", *segment);
                    return nullptr;
                }

                if (index >= length) {
                    LOG_DEBUG("cannot lookup an array element with \"{1}\": expected an integral value between 0 and {2} (inclusive).", *segment, length - 1);
                    return nullptr;
                }

                val = ruby.rb_ary_entry(val, index);
            } else if (ruby.is_hash(val)) {
                // if we're anything but an array, we look up by name
                auto key = ruby.utf8_value(*segment);
                auto result = ruby.rb_hash_lookup(val, key);
                if (ruby.is_nil(result)) {
                    // We also want to try looking up as a symbol
                    key = ruby.to_symbol(*segment);
                    result = ruby.rb_hash_lookup(val, key);
                }
                val = result;
            } else {
                LOG_DEBUG("cannot lookup element \"{1}\": container is not an array or hash", *segment);
            }
            if (ruby.is_nil(val)) {
                return nullptr;
            }
        }
        return rb_value->wrap_child(val, move(key));
    }

    void uninitialize()
    {
        api& ruby = api::instance();
        ruby.uninitialize();
    }
}}  // namespace facter::ruby
