# frozen_string_literal: true

module Facter
  module Resolvers
    module Solaris
      class Mountpoints < BaseResolver
        include Facter::Util::Resolvers::FilesystemHelper
        init_resolver

        class << self
          private

          def post_resolve(fact_name, _options)
            @fact_list.fetch(fact_name) { read_mounts(fact_name) }
          end

          def root_device
            cmdline = Facter::Util::FileHelper.safe_read('/proc/cmdline')
            match = cmdline.match(/root=([^\s]+)/)
            match&.captures&.first
          end

          def compute_device(device)
            # If the "root" device, lookup the actual device from the kernel options
            # This is done because not all systems symlink /dev/root
            device = root_device if device == '/dev/root'
            device
          end

          def read_mounts(fact_name) # rubocop:disable Metrics/AbcSize, Metrics/MethodLength
            mounts = []
            Facter::Util::Resolvers::FilesystemHelper.read_mountpoints.each do |fs|
              device = compute_device(fs.name)
              filesystem = fs.mount_type
              path = fs.mount_point
              options = fs.options.split(',').map(&:strip)

              stats = Facter::Util::Resolvers::FilesystemHelper.read_mountpoint_stats(path)
              size_bytes = stats.bytes_total.abs
              available_bytes = stats.bytes_available.abs

              used_bytes = stats.bytes_used.abs
              total_bytes = used_bytes + available_bytes
              capacity = Facter::Util::Resolvers::FilesystemHelper.compute_capacity(used_bytes, total_bytes)

              size = Facter::Util::Facts::UnitConverter.bytes_to_human_readable(size_bytes)
              available = Facter::Util::Facts::UnitConverter.bytes_to_human_readable(available_bytes)
              used = Facter::Util::Facts::UnitConverter.bytes_to_human_readable(used_bytes)

              mounts << Hash[Facter::Util::Resolvers::FilesystemHelper::MOUNT_KEYS
                             .zip(Facter::Util::Resolvers::FilesystemHelper::MOUNT_KEYS
                .map { |v| binding.local_variable_get(v) })]
            end
            @fact_list[:mountpoints] = mounts
            @fact_list[fact_name]
          end
        end
      end
    end
  end
end
