from setuptools import setup


setup(
    name="bim_ros_bridge",
    version="0.1.0",
    packages=["bim_ros_bridge"],
    data_files=[
        ("share/ament_index/resource_index/packages", ["resource/bim_ros_bridge"]),
        ("share/bim_ros_bridge", ["package.xml"]),
        ("share/bim_ros_bridge/launch", ["launch/bridge.launch.py"]),
    ],
    install_requires=["setuptools"],
    zip_safe=True,
    entry_points={"console_scripts": ["bridge = bim_ros_bridge.node:main"]},
)
