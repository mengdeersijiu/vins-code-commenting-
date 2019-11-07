#include <stdio.h>
#include <queue>
#include <map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <ros/ros.h>
#include <cv_bridge/cv_bridge.h>
#include <opencv2/opencv.hpp>

#include "estimator.h"
#include "parameters.h"
#include "utility/visualization.h"


Estimator estimator;

std::condition_variable con;
double current_time = -1;
queue<sensor_msgs::ImuConstPtr> imu_buf;
queue<sensor_msgs::PointCloudConstPtr> feature_buf;
queue<sensor_msgs::PointCloudConstPtr> relo_buf;
int sum_of_wait = 0;

std::mutex m_buf;
std::mutex m_state;
std::mutex i_buf;
std::mutex m_estimator;

double latest_time;
//tmp 里程计数据
Eigen::Vector3d tmp_P;
Eigen::Quaterniond tmp_Q;
Eigen::Vector3d tmp_V;
Eigen::Vector3d tmp_Ba;
Eigen::Vector3d tmp_Bg;
Eigen::Vector3d acc_0;
Eigen::Vector3d gyr_0;
bool init_feature = 0;
bool init_imu = 1;
double last_imu_t = 0;

//这一段并不是预积分公式，只是和预积分相关的一部分，// 通过IMU测量值积分更新里程计信息
//tmp_P、tmp_V、tmp_Q是在非线性优化之后获得的世界坐标系下准确的位置、速度和位姿
void predict(const sensor_msgs::ImuConstPtr &imu_msg)
{
    double t = imu_msg->header.stamp.toSec();
    if (init_imu)
    {
        latest_time = t;
        init_imu = 0;
        return;
    }
    double dt = t - latest_time;
    latest_time = t;

    double dx = imu_msg->linear_acceleration.x;
    double dy = imu_msg->linear_acceleration.y;
    double dz = imu_msg->linear_acceleration.z;
    Eigen::Vector3d linear_acceleration{dx, dy, dz};

    double rx = imu_msg->angular_velocity.x;
    double ry = imu_msg->angular_velocity.y;
    double rz = imu_msg->angular_velocity.z;
    Eigen::Vector3d angular_velocity{rx, ry, rz};

    Eigen::Vector3d un_acc_0 = tmp_Q * (acc_0 - tmp_Ba) - estimator.g;

    Eigen::Vector3d un_gyr = 0.5 * (gyr_0 + angular_velocity) - tmp_Bg;
    tmp_Q = tmp_Q * Utility::deltaQ(un_gyr * dt);

    Eigen::Vector3d un_acc_1 = tmp_Q * (linear_acceleration - tmp_Ba) - estimator.g;

    Eigen::Vector3d un_acc = 0.5 * (un_acc_0 + un_acc_1);
//这里的计算得到的pvq是估计值. 作用是与下面预积分计算得到的pvq（考虑了观测噪声和偏置）做差得到残差???//tmp只看到被用在可视化了
    tmp_P = tmp_P + dt * tmp_V + 0.5 * dt * dt * un_acc;
    tmp_V = tmp_V + dt * un_acc;

    acc_0 = linear_acceleration;
    gyr_0 = angular_velocity;
}

// 当处理完measurements中的所有数据后，如果VINS系统正常完成滑动窗口优化，那么需要用优化后的结果更新里程计数据
void update()
{
    TicToc t_predict;
    latest_time = current_time;
    // 首先获取滑动窗口中最新帧的P、V、Q
    tmp_P = estimator.Ps[WINDOW_SIZE];//tmp_P=Ps[10]
    tmp_Q = estimator.Rs[WINDOW_SIZE];
    tmp_V = estimator.Vs[WINDOW_SIZE];
    tmp_Ba = estimator.Bas[WINDOW_SIZE];
    tmp_Bg = estimator.Bgs[WINDOW_SIZE];
    acc_0 = estimator.acc_0;
    gyr_0 = estimator.gyr_0;

    // 滑动窗口中最新帧并不是当前帧，中间隔着缓存队列的数据，所以还需要使用缓存队列中的IMU数据进行积分得到当前帧的里程计信息
    queue<sensor_msgs::ImuConstPtr> tmp_imu_buf = imu_buf;
    for (sensor_msgs::ImuConstPtr tmp_imu_msg; !tmp_imu_buf.empty(); tmp_imu_buf.pop())
        predict(tmp_imu_buf.front());

}
//getMeasurements()函数将对imu和图像数据进行初步对齐得到的数据结构，确保图像关联着对应时间戳内的所有IMU数据
//sensor_msgs::PointCloudConstPtr 表示某一帧图像的feature_points
//std::vector<sensor_msgs::ImuConstPtr> 表示当前帧和上一帧时间间隔中的所有IMU数据
//将两者组合成一个数据结构，并构建元素为这种结构的vector进行存储
std::vector<std::pair<std::vector<sensor_msgs::ImuConstPtr>, sensor_msgs::PointCloudConstPtr>> getMeasurements()
{
    std::vector<std::pair<std::vector<sensor_msgs::ImuConstPtr>, sensor_msgs::PointCloudConstPtr>> measurements;

    // 直到把imu_buf或者feature_buf中的数据全部取出，才会退出while循环
    while (true)
    {
        if (imu_buf.empty() || feature_buf.empty())
            return measurements;

        // imu_buf队尾元素的时间戳，早于或等于feature_buf队首元素的时间戳（时间偏移补偿后），则需要等待接收IMU数据
        if (!(imu_buf.back()->header.stamp.toSec() > feature_buf.front()->header.stamp.toSec() + estimator.td))
        {
            //ROS_WARN("wait for imu, only should happen at the beginning");
            sum_of_wait++;
            return measurements;
        }

        // imu_buf队首元素的时间戳，晚于或等于feature_buf队首元素的时间戳（时间偏移补偿后），则需要剔除feature_buf队首多余的特征点数据
        if (!(imu_buf.front()->header.stamp.toSec() < feature_buf.front()->header.stamp.toSec() + estimator.td))
        {
            ROS_WARN("throw img, only should happen at the beginning");
            feature_buf.pop();// 剔除feature_buf队首的数据
            continue;
        }
        sensor_msgs::PointCloudConstPtr img_msg = feature_buf.front();// 读取feature_buf队首的数据
        feature_buf.pop();

        std::vector<sensor_msgs::ImuConstPtr> IMUs;
        // 一帧图像特征点数据，对应多帧imu数据,把它们进行对应，然后塞入measurements
        // 一帧图像特征点数据，与它和上一帧图像特征点数据之间的时间间隔内所有的IMU数据，以及时间戳晚于当前帧图像的第一帧IMU数据对应
        // 如下图所示：
        //   *       *      *       *      *       *      *      *        （IMU数据）
        //                                                     |          （图像特征点数据）
        while (imu_buf.front()->header.stamp.toSec() < img_msg->header.stamp.toSec() + estimator.td)
        {
            IMUs.emplace_back(imu_buf.front());
            imu_buf.pop();
        }
        IMUs.emplace_back(imu_buf.front());// 时间戳晚于当前帧图像的第一帧IMU数据
        if (IMUs.empty())
            ROS_WARN("no imu between two image");
        measurements.emplace_back(IMUs, img_msg);
    }
    return measurements;
}
//主要功能是往imubuf里存数据的
void imu_callback(const sensor_msgs::ImuConstPtr &imu_msg)
{
    if (imu_msg->header.stamp.toSec() <= last_imu_t)
    {
        ROS_WARN("imu message in disorder!");
        return;
    }
    last_imu_t = imu_msg->header.stamp.toSec();
//防止存入imu数据时 imu_buf冲突？
    m_buf.lock();
    imu_buf.push(imu_msg);
    m_buf.unlock();
    con.notify_one();//完成将img_msg放入imu_buf的操作后唤醒作用于process线程中的获取观测值数据的函数getMeasurements()：
                     // 读取缓存imu_buf和feature_buf中的观测数据

    // 通过IMU测量值积分更新并发布里程计信息
    last_imu_t = imu_msg->header.stamp.toSec();// 这一行代码似乎重复了，上面有着一模一样的代码

    {
        std::lock_guard<std::mutex> lg(m_state);
        predict(imu_msg);//个人理解：每来一个imu数据 就拿这个数据来累加在PVQ上，而且是去除bias影响并经过estimator处理的PVQ
        std_msgs::Header header = imu_msg->header;
        header.frame_id = "world";
        // VINS初始化已完成，正处于滑动窗口非线性优化状态，如果VINS还在初始化，则不发布里程计信息
        if (estimator.solver_flag == Estimator::SolverFlag::NON_LINEAR)
            // 发布里程计信息，发布频率很高（与IMU数据同频），每次获取IMU数据都会及时进行更新，而且发布的是当前的里程计信息。
            pubLatestOdometry(tmp_P, tmp_Q, tmp_V, header);//所以tmp_PVQ只是拿来可视化了吗？
        // 还有一个pubOdometry()函数，似乎也是发布里程计信息，
        // 但是它是在estimator每次处理完一帧图像特征点数据后才发布的，有延迟，而且频率也不高（至多与图像同频）
    }
}


void feature_callback(const sensor_msgs::PointCloudConstPtr &feature_msg)
{
    if (!init_feature)
    {
        //skip the first detected feature, which doesn't contain optical flow speed
        init_feature = 1;
        return;
    }
    // 将图像特征点数据存入图像特征点数据缓存队列feature_buf
    m_buf.lock();
    feature_buf.push(feature_msg);
    m_buf.unlock();
    con.notify_one();// 唤醒getMeasurements()读取缓存imu_buf和feature_buf中的观测数据
}

void restart_callback(const std_msgs::BoolConstPtr &restart_msg)
{
    if (restart_msg->data == true)
    {
        ROS_WARN("restart the estimator!");
        m_buf.lock();
        while(!feature_buf.empty())
            feature_buf.pop();
        while(!imu_buf.empty())
            imu_buf.pop();
        m_buf.unlock();
        m_estimator.lock();
        estimator.clearState();
        estimator.setParameter();
        m_estimator.unlock();
        current_time = -1;
        last_imu_t = 0;
    }
    return;
}

void relocalization_callback(const sensor_msgs::PointCloudConstPtr &points_msg)
{
    //printf("relocalization callback! \n");
    m_buf.lock();
    relo_buf.push(points_msg);
    m_buf.unlock();
}

// thread: visual-inertial odometry
// process()是measurement_process线程的线程函数，在process()中处理VIO后端，包括IMU预积分、松耦合初始化和local BA
void process()//内容比较多的部分
{
    while (true)
    {
        //sensor_msgs::PointCloudConstPtr 表示某一帧图像的feature_points
        //std::vector<sensor_msgs::ImuConstPtr> 表示当前帧和上一帧时间间隔中的所有IMU数据
        //将两者组合成一个数据结构，并构建元素为这种结构的vector进行存储
        std::vector<std::pair<std::vector<sensor_msgs::ImuConstPtr>, sensor_msgs::PointCloudConstPtr>> measurements;


        // unique_lock对象lk以独占所有权的方式管理mutex对象m_buf的上锁和解锁操作，所谓独占所有权，就是没有其他的 unique_lock对象同时拥有m_buf的所有权，
        // 新创建的unique_lock对象lk管理Mutex对象m_buf，并尝试调用m_buf.lock()对Mutex对象m_buf进行上锁，如果此时另外某个unique_lock对象已经管理了该Mutex对象m_buf,
        // 则当前线程将会被阻塞；如果此时m_buf本身就处于上锁状态，当前线程也会被阻塞（我猜的）。
        // 在unique_lock对象lk的声明周期内，它所管理的锁对象m_buf会一直保持上锁状态
        std::unique_lock<std::mutex> lk(m_buf);
        // [&]{return (measurements = getMeasurements()).size() != 0;}是lamda表达式（匿名函数）
        // 先调用匿名函数，从缓存队列中读取IMU数据和图像特征点数据，如果measurements为空，则匿名函数返回false，调用wait(lock)，
        // 释放m_buf（为了使图像和IMU回调函数可以访问缓存队列），阻塞当前线程，等待被con.notify_one()唤醒
        // 直到measurements不为空时（成功从缓存队列获取数据），匿名函数返回true，则可以退出while循环。
        con.wait(lk, [&]
                 {
            return (measurements = getMeasurements()).size() != 0;
                 });
        lk.unlock();// 从缓存队列中读取数据完成，解锁
        m_estimator.lock();
        //上面一堆操作 下面是 把存着的一组imu和对应的单个图像特征的一对对的数据拿来用
        for (auto &measurement : measurements)
        {
            auto img_msg = measurement.second;
            double dx = 0, dy = 0, dz = 0, rx = 0, ry = 0, rz = 0;

            //遍历该组中的各帧imu数据，进行预积分
            for (auto &imu_msg : measurement.first)
            {
                double t = imu_msg->header.stamp.toSec();// 最新IMU数据的时间戳
                double img_t = img_msg->header.stamp.toSec() + estimator.td;// 图像特征点数据的时间戳，补偿了通过优化得到的一个时间偏移
                if (t <= img_t)// 补偿时间偏移后，图像特征点数据的时间戳晚于或等于IMU数据的时间戳
                {
                    if (current_time < 0)// 第一次接收IMU数据时会出现这种情况
                        current_time = t;
                    double dt = t - current_time;
                    ROS_ASSERT(dt >= 0);
                    current_time = t;// 更新最近一次接收的IMU数据的时间戳
                    dx = imu_msg->linear_acceleration.x;
                    dy = imu_msg->linear_acceleration.y;
                    dz = imu_msg->linear_acceleration.z;
                    rx = imu_msg->angular_velocity.x;
                    ry = imu_msg->angular_velocity.y;
                    rz = imu_msg->angular_velocity.z;
                    estimator.processIMU(dt, Vector3d(dx, dy, dz), Vector3d(rx, ry, rz));
                    //printf("imu: dt:%f a: %f %f %f w: %f %f %f\n",dt, dx, dy, dz, rx, ry, rz);

                }
                // 时间戳晚于图像特征点数据（时间偏移补偿后）的第一帧IMU数据（也是一组measurement中的唯一一帧），
                // 对IMU数据进行插值，得到图像帧时间戳对应的IMU数据
                // 时间戳的对应关系如下图所示：
                //                                            current_time         t
                // *               *               *               *               *     （IMU数据）
                //                                                          |            （图像特征点数据）
                //                                                        img_t
                else
                {
                    double dt_1 = img_t - current_time;
                    double dt_2 = t - img_t;
                    current_time = img_t;
                    ROS_ASSERT(dt_1 >= 0);
                    ROS_ASSERT(dt_2 >= 0);
                    ROS_ASSERT(dt_1 + dt_2 > 0);
                    double w1 = dt_2 / (dt_1 + dt_2);
                    double w2 = dt_1 / (dt_1 + dt_2);
                    dx = w1 * dx + w2 * imu_msg->linear_acceleration.x;
                    dy = w1 * dy + w2 * imu_msg->linear_acceleration.y;
                    dz = w1 * dz + w2 * imu_msg->linear_acceleration.z;
                    rx = w1 * rx + w2 * imu_msg->angular_velocity.x;
                    ry = w1 * ry + w2 * imu_msg->angular_velocity.y;
                    rz = w1 * rz + w2 * imu_msg->angular_velocity.z;
                    estimator.processIMU(dt_1, Vector3d(dx, dy, dz), Vector3d(rx, ry, rz));
                    //printf("dimu: dt:%f a: %f %f %f w: %f %f %f\n",dt_1, dx, dy, dz, rx, ry, rz);
                }
            }
            // set relocalization frame  回环检测相关
            sensor_msgs::PointCloudConstPtr relo_msg = NULL;
            while (!relo_buf.empty())
            {
                relo_msg = relo_buf.front();
                relo_buf.pop();
            }
            if (relo_msg != NULL)
            {
                vector<Vector3d> match_points;
                double frame_stamp = relo_msg->header.stamp.toSec();
                for (unsigned int i = 0; i < relo_msg->points.size(); i++)
                {
                    Vector3d u_v_id;
                    u_v_id.x() = relo_msg->points[i].x;
                    u_v_id.y() = relo_msg->points[i].y;
                    u_v_id.z() = relo_msg->points[i].z;
                    match_points.push_back(u_v_id);
                }
                Vector3d relo_t(relo_msg->channels[0].values[0], relo_msg->channels[0].values[1], relo_msg->channels[0].values[2]);
                Quaterniond relo_q(relo_msg->channels[0].values[3], relo_msg->channels[0].values[4], relo_msg->channels[0].values[5], relo_msg->channels[0].values[6]);
                Matrix3d relo_r = relo_q.toRotationMatrix();
                int frame_index;
                frame_index = relo_msg->channels[0].values[7];
                estimator.setReloFrame(frame_stamp, frame_index, match_points, relo_t, relo_r);
            }

            ROS_DEBUG("processing vision data with stamp %f \n", img_msg->header.stamp.toSec());

            TicToc t_s;
            //map是由很多pair组成，下面map的键值是feature_id，实值是vector<pair<int, Eigen::Matrix<double, 7, 1>>>
            map<int, vector<pair<int, Eigen::Matrix<double, 7, 1>>>> image;//这个数据结构只被processImage调用
            for (unsigned int i = 0; i < img_msg->points.size(); i++)
            {
                int v = img_msg->channels[0].values[i] + 0.5;
                int feature_id = v / NUM_OF_CAM;
                int camera_id = v % NUM_OF_CAM;
                double x = img_msg->points[i].x;
                double y = img_msg->points[i].y;
                double z = img_msg->points[i].z;
                double p_u = img_msg->channels[1].values[i];
                double p_v = img_msg->channels[2].values[i];
                double velocity_x = img_msg->channels[3].values[i];
                double velocity_y = img_msg->channels[4].values[i];
                ROS_ASSERT(z == 1);
                Eigen::Matrix<double, 7, 1> xyz_uv_velocity;
                xyz_uv_velocity << x, y, z, p_u, p_v, velocity_x, velocity_y;
                //建立每个特征点(camera_id,[x,y,z,u,v,vx,vy])构成的map，索引为feature_id
                image[feature_id].emplace_back(camera_id,  xyz_uv_velocity);
            }
            estimator.processImage(image, img_msg->header);

            double whole_t = t_s.toc();
            printStatistics(estimator, whole_t);
            std_msgs::Header header = img_msg->header;
            header.frame_id = "world";

            pubOdometry(estimator, header);
            pubKeyPoses(estimator, header);
            pubCameraPose(estimator, header);
            pubPointCloud(estimator, header);
            pubTF(estimator, header);
            pubKeyframe(estimator);
            if (relo_msg != NULL)
                pubRelocalization(estimator);
            //ROS_ERROR("end: %f, at %f", img_msg->header.stamp.toSec(), ros::Time::now().toSec());
        }
        m_estimator.unlock();
        m_buf.lock();
        m_state.lock();
        if (estimator.solver_flag == Estimator::SolverFlag::NON_LINEAR)
            // VINS系统完成滑动窗口优化后，用优化后的结果，更新里程计数据
            update();
        m_state.unlock();
        m_buf.unlock();
    }
}

int main(int argc, char **argv)
{
    ros::init(argc, argv, "vins_estimator");
    ros::NodeHandle n("~");
    ros::console::set_logger_level(ROSCONSOLE_DEFAULT_NAME, ros::console::levels::Info);
    readParameters(n);
    estimator.setParameter();
#ifdef EIGEN_DONT_PARALLELIZE
    ROS_DEBUG("EIGEN_DONT_PARALLELIZE");
#endif
    ROS_WARN("waiting for image and imu...");

    registerPub(n);

    ros::Subscriber sub_imu = n.subscribe(IMU_TOPIC, 2000, imu_callback, ros::TransportHints().tcpNoDelay());
    ros::Subscriber sub_image = n.subscribe("/feature_tracker/feature", 2000, feature_callback);
    ros::Subscriber sub_restart = n.subscribe("/feature_tracker/restart", 2000, restart_callback);
    ros::Subscriber sub_relo_points = n.subscribe("/pose_graph/match_points", 2000, relocalization_callback);

    std::thread measurement_process{process};
    ros::spin();

    return 0;
}
